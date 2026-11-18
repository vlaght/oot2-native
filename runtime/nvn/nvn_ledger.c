// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The oot2-native contributors
//
// The ledger's tracker and writer (oot2/ledger.h, architecture D21). The tracker follows the records of a replay and
// makes a row at each draw and dispatch for the stages that run; the rows are deduplicated in memory, and the file is
// written by ledger_flush at a safe point, never from the tracker.
//
// The row is made first in a fixed binary form (RowBin), hashed and looked up, which is all a draw that was seen before
// costs; only a new row is formatted. The file is JSON Lines, one row an object, the first line a header:
//   {"key":"<key16>.<variant>","kind":"ps","answers":{"fout":{"0":"uint"},"tex":{"0":"2darray"}},"digest":"<64 hex>|0",
//    "attach":{"color":[<NVNformat>,..],"depth":<NVNformat>|null,"samples":<n>}}
// "answers" is the canonical JSON text the translator job takes (sorted keys, no spaces) and "digest" the SHA-256 of
// that text, or 0 when there are no answers, as tools/shaders/cache_pack.py answers_digest computes it. Rows are
// written sorted, so the file does not depend on the order the game met them in.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn_api.h"
#include "oot2/bench_timers.h"
#include "oot2/ledger.h"
#include "oot2/loader.h"
#include "oot2/nvn_program.h"
#include "oot2/nvn_tex.h"
#include "oot2/replay_count.h"
#include "plat.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef unsigned long long ull;

// Texture types, as the translator's answers name them (xlate/host/ResourceInfo.cs TypeNames).
enum { T_NONE, T_1D, T_1DA, T_BUF, T_2D, T_2DA, T_2DMS, T_2DMSA, T_3D, T_CUBE, T_CUBEA, T_COUNT };
static const char *const TYPE_NAME[T_COUNT] = {"",   "1d",   "1darray", "buffer", "2d", "2darray",
                                               "2dms", "2dmsarray", "3d", "cube", "cubearray"};
enum { A_NONE, A_FLOAT, A_SINT, A_UINT };
static const char *const CLASS_NAME[4] = {"", "float", "sint", "uint"};
static const uint8_t KIND_STAGE[NVN_STAGE_KINDS] = {0, 1, 5, 6, 7};   // NVN stage number of each stage kind

static uint8_t type_of_target(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return T_1D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return T_1DA;
    case NVN_TEXTURE_TARGET_BUFFER: return T_BUF;
    case NVN_TEXTURE_TARGET_2D: case NVN_TEXTURE_TARGET_RECTANGLE: return T_2D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: return T_2DA;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: return T_2DMS;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return T_2DMSA;
    case NVN_TEXTURE_TARGET_3D: return T_3D;
    case NVN_TEXTURE_TARGET_CUBEMAP: return T_CUBE;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return T_CUBEA;
    default: return T_NONE;
    }
}

// How a shader reads a vertex attribute or writes a colour target of this format: NVN's integer formats are read as
// integers, every other (normalised, scaled, float, and the _UI2F and _I2F conversions) as float.
static uint8_t class_of_format(int f) {
    switch (f) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_R16UI: case NVN_FORMAT_R32UI: case NVN_FORMAT_RG8UI:
    case NVN_FORMAT_RG16UI: case NVN_FORMAT_RG32UI: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGB16UI:
    case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGBA8UI: case NVN_FORMAT_RGBA16UI: case NVN_FORMAT_RGBA32UI:
    case NVN_FORMAT_RGB10A2UI: case NVN_FORMAT_RGBX8UI: case NVN_FORMAT_RGBX16UI: case NVN_FORMAT_RGBX32UI:
        return A_UINT;
    case NVN_FORMAT_R8I: case NVN_FORMAT_R16I: case NVN_FORMAT_R32I: case NVN_FORMAT_RG8I: case NVN_FORMAT_RG16I:
    case NVN_FORMAT_RG32I: case NVN_FORMAT_RGB8I: case NVN_FORMAT_RGB16I: case NVN_FORMAT_RGB32I:
    case NVN_FORMAT_RGBA8I: case NVN_FORMAT_RGBA16I: case NVN_FORMAT_RGBA32I: case NVN_FORMAT_RGB10A2I:
    case NVN_FORMAT_RGBX8I: case NVN_FORMAT_RGBX16I: case NVN_FORMAT_RGBX32I:
        return A_SINT;
    default: return A_FLOAT;
    }
}

// The translator's TextureFormat name of a storage image's format (xlate/host/Job.cs "fmt"); "Unknown" for a format the
// translator has no storage format for.
static const char *image_format_name(int f) {
    switch (f) {
    case NVN_FORMAT_R8: return "R8Unorm";
    case NVN_FORMAT_R8SN: return "R8Snorm";
    case NVN_FORMAT_R8UI: return "R8Uint";
    case NVN_FORMAT_R8I: return "R8Sint";
    case NVN_FORMAT_R16F: return "R16Float";
    case NVN_FORMAT_R16: return "R16Unorm";
    case NVN_FORMAT_R16SN: return "R16Snorm";
    case NVN_FORMAT_R16UI: return "R16Uint";
    case NVN_FORMAT_R16I: return "R16Sint";
    case NVN_FORMAT_R32F: return "R32Float";
    case NVN_FORMAT_R32UI: return "R32Uint";
    case NVN_FORMAT_R32I: return "R32Sint";
    case NVN_FORMAT_RG8: return "R8G8Unorm";
    case NVN_FORMAT_RG8SN: return "R8G8Snorm";
    case NVN_FORMAT_RG8UI: return "R8G8Uint";
    case NVN_FORMAT_RG8I: return "R8G8Sint";
    case NVN_FORMAT_RG16F: return "R16G16Float";
    case NVN_FORMAT_RG16: return "R16G16Unorm";
    case NVN_FORMAT_RG16SN: return "R16G16Snorm";
    case NVN_FORMAT_RG16UI: return "R16G16Uint";
    case NVN_FORMAT_RG16I: return "R16G16Sint";
    case NVN_FORMAT_RG32F: return "R32G32Float";
    case NVN_FORMAT_RG32UI: return "R32G32Uint";
    case NVN_FORMAT_RG32I: return "R32G32Sint";
    case NVN_FORMAT_RGBA8: return "R8G8B8A8Unorm";
    case NVN_FORMAT_RGBA8SN: return "R8G8B8A8Snorm";
    case NVN_FORMAT_RGBA8UI: return "R8G8B8A8Uint";
    case NVN_FORMAT_RGBA8I: return "R8G8B8A8Sint";
    case NVN_FORMAT_RGBA16F: return "R16G16B16A16Float";
    case NVN_FORMAT_RGBA16: return "R16G16B16A16Unorm";
    case NVN_FORMAT_RGBA16SN: return "R16G16B16A16Snorm";
    case NVN_FORMAT_RGBA16UI: return "R16G16B16A16Uint";
    case NVN_FORMAT_RGBA16I: return "R16G16B16A16Sint";
    case NVN_FORMAT_RGBA32F: return "R32G32B32A32Float";
    case NVN_FORMAT_RGBA32UI: return "R32G32B32A32Uint";
    case NVN_FORMAT_RGBA32I: return "R32G32B32A32Sint";
    case NVN_FORMAT_RGB10A2: return "R10G10B10A2Unorm";
    case NVN_FORMAT_RGB10A2UI: return "R10G10B10A2Uint";
    case NVN_FORMAT_R11G11B10F: return "R11G11B10Float";
    default: return "Unknown";
    }
}

// ---------- the texture pool's descriptors: (table address, ID) -> type and format
typedef struct { uint64_t pool; uint32_t id; uint8_t used, type; uint16_t fmt; } Desc;
static PlatMutex g_dlock = PLAT_MUTEX_INIT;
static Desc *g_desc;
static size_t g_dcap, g_dn;

static size_t desc_slot(uint64_t pool, uint32_t id, size_t mask) {
    uint64_t h = (pool >> 5) * 0x9E3779B97F4A7C15ull ^ (uint64_t)id * 0xC2B2AE3D27D4EB4Full;
    return (size_t)(h >> 29) & mask;
}

void ledger_desc_register(const dpool_ref *pool, int id, const tex_t *t, const view_t *v) {
    uint8_t type = type_of_target(v && v->set_target ? v->target : t->d.target);
    uint16_t fmt = (uint16_t)(v && v->set_format ? v->format : t->d.format);
    plat_mutex_lock(&g_dlock);
    if ((g_dn + 1) * 2 > g_dcap) {
        size_t cap = g_dcap ? 2 * g_dcap : 4096;
        Desc *n = calloc(cap, sizeof *n);
        if (!n) rt_fatal("ledger: out of memory for %zu texture descriptors", cap);
        for (size_t i = 0; i < g_dcap; i++) {
            if (!g_desc[i].used) continue;
            size_t j = desc_slot(g_desc[i].pool, g_desc[i].id, cap - 1);
            while (n[j].used) j = (j + 1) & (cap - 1);
            n[j] = g_desc[i];
        }
        free(g_desc);
        g_desc = n, g_dcap = cap;
    }
    size_t j = desc_slot(pool->addr, (uint32_t)id, g_dcap - 1);
    while (g_desc[j].used && !(g_desc[j].pool == pool->addr && g_desc[j].id == (uint32_t)id)) j = (j + 1) & (g_dcap - 1);
    if (!g_desc[j].used) g_dn++;
    g_desc[j] = (Desc){pool->addr, (uint32_t)id, 1, type, fmt};
    plat_mutex_unlock(&g_dlock);
}

static int desc_find(uint64_t pool, uint32_t id, uint8_t *type, uint16_t *fmt) {
    int found = 0;
    plat_mutex_lock(&g_dlock);
    if (g_dcap) {
        size_t j = desc_slot(pool, id, g_dcap - 1);
        while (g_desc[j].used) {
            if (g_desc[j].pool == pool && g_desc[j].id == id) {
                *type = g_desc[j].type, *fmt = g_desc[j].fmt, found = 1;
                break;
            }
            j = (j + 1) & (g_dcap - 1);
        }
    }
    plat_mutex_unlock(&g_dlock);
    return found;
}

// ---------- rows
// No padding, so hashing and comparing the bytes compares the row (asserted below).
typedef struct {
    const nvn_stage *st;
    uint16_t color_fmt[LEDGER_TARGETS];
    uint16_t img_fmt[LEDGER_IMG_SLOTS];
    int32_t depth_fmt;
    uint8_t tex[LEDGER_TEX_SLOTS];
    uint8_t img_type[LEDGER_IMG_SLOTS];
    uint8_t attr[LEDGER_ATTRS];
    uint8_t fout[LEDGER_TARGETS];
    uint8_t ncolor, samples, raster;
} RowBin;
_Static_assert(sizeof(RowBin) == 8 + 2 * LEDGER_TARGETS + 2 * LEDGER_IMG_SLOTS + 4 + LEDGER_TEX_SLOTS + LEDGER_IMG_SLOTS +
                                     LEDGER_ATTRS + LEDGER_TARGETS + 3,
               "RowBin has no padding");

typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(b->s ? b->s + b->n : NULL, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k < 0) rt_fatal("ledger: a row could not be formatted");
        if ((size_t)k < b->cap - b->n) {
            b->n += (size_t)k;
            return;
        }
        size_t cap = b->cap ? 2 * b->cap : 512;
        while (cap - b->n <= (size_t)k) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s) rt_fatal("ledger: out of memory for a row");
        b->s = s, b->cap = cap;
    }
}

static int cmp_slot_text(const void *a, const void *b) {
    char x[16], y[16];
    snprintf(x, sizeof x, "%d", *(const int *)a);
    snprintf(y, sizeof y, "%d", *(const int *)b);
    return strcmp(x, y);
}

// The slots 0 to LEDGER_TEX_SLOTS - 1 in the order of the text of their numbers ("0", "1", "10", "11", ..., "2"), which
// json.dumps(sort_keys=True) writes them in. The order does not change, so it is made once, not by a sort for every map.
static int g_slot_order[LEDGER_TEX_SLOTS];
static int g_slot_order_ready;

static void slot_order_init(void) {
    if (__atomic_load_n(&g_slot_order_ready, __ATOMIC_ACQUIRE)) return;
    int o[LEDGER_TEX_SLOTS];
    for (int i = 0; i < LEDGER_TEX_SLOTS; i++) o[i] = i;
    qsort(o, LEDGER_TEX_SLOTS, sizeof o[0], cmp_slot_text);
    memcpy(g_slot_order, o, sizeof o);
    __atomic_store_n(&g_slot_order_ready, 1, __ATOMIC_RELEASE);
}

// One answer map, keys sorted as the text of their numbers (json.dumps(sort_keys=True) sorts strings).
static int put_map(Buf *b, int first, const char *name, const uint8_t *v, int n, int kind, const RowBin *r) {
    int slots[LEDGER_TEX_SLOTS], k = 0;
    slot_order_init();
    for (int oi = 0; oi < LEDGER_TEX_SLOTS; oi++) {
        int i = g_slot_order[oi];
        if (i < n && v[i]) slots[k++] = i;
    }
    if (!k) return first;
    bput(b, "%s\"%s\":{", first ? "" : ",", name);
    for (int i = 0; i < k; i++) {
        int s = slots[i];
        bput(b, "%s\"%d\":", i ? "," : "", s);
        if (kind == 0) bput(b, "\"%s\"", TYPE_NAME[v[s]]);
        else if (kind == 1) bput(b, "\"%s\"", CLASS_NAME[v[s]]);
        else bput(b, "{\"fmt\":\"%s\",\"type\":\"%s\"}", image_format_name(r->img_fmt[s]), TYPE_NAME[v[s]]);
    }
    bput(b, "}");
    return 0;
}

static char *answers_text(const RowBin *r, uint8_t digest[32]) {
    Buf ans = {0};
    int first = 1;
    first = put_map(&ans, first, "attr", r->attr, LEDGER_ATTRS, 1, r);
    first = put_map(&ans, first, "fout", r->fout, LEDGER_TARGETS, 1, r);
    first = put_map(&ans, first, "img", r->img_type, LEDGER_IMG_SLOTS, 2, r);
    first = put_map(&ans, first, "tex", r->tex, LEDGER_TEX_SLOTS, 0, r);
    Buf wrapped = {0};
    bput(&wrapped, "{%s}", ans.s ? ans.s : "");
    free(ans.s);
    if (first) memset(digest, 0, 32);
    else BT_TIME(BT_S1_SHA, rt_sha256(wrapped.s, wrapped.n, digest));
    return wrapped.s;
}

static char *format_row(const RowBin *r) {
    uint8_t digest[32];
    char text_digest[65] = "0";
    char *answers = answers_text(r, digest);
    Buf text = {0};
    if (memcmp(digest, (uint8_t[32]){0}, 32)) {
        for (int i = 0; i < 32; i++) snprintf(text_digest + 2 * i, 3, "%02x", digest[i]);
        bput(&text, "{\"key\":\"%s\",\"kind\":\"%s\",\"answers\":%s,\"digest\":\"%s\"", r->st->key,
             nvn_stage_kind_name(r->st->kind), answers, text_digest);
    } else {
        bput(&text, "{\"key\":\"%s\",\"kind\":\"%s\",\"answers\":{},\"digest\":\"0\"", r->st->key,
             nvn_stage_kind_name(r->st->kind));
    }
    free(answers);
    if (r->raster) {
        bput(&text, ",\"attach\":{\"color\":[");
        for (int i = 0; i < r->ncolor; i++) bput(&text, "%s%u", i ? "," : "", r->color_fmt[i]);
        if (r->depth_fmt >= 0) bput(&text, "],\"depth\":%d", r->depth_fmt);
        else bput(&text, "],\"depth\":null");
        bput(&text, ",\"samples\":%u}", r->samples);
    }
    bput(&text, "}");
    return text.s;
}

// ---------- the sets
static PlatMutex g_lock = PLAT_MUTEX_INIT;
typedef struct { RowBin row; uint64_t hash; } BinEntry;
static BinEntry *g_bin;                  // rows seen in this run, in binary form
static size_t g_bin_n, g_bin_cap;
static uint32_t *g_bin_idx;              // open-addressing index into g_bin, entry + 1; 0 empty
static size_t g_bin_idx_cap;
static char **g_text;                    // every row held, formatted, in the order added
static uint64_t *g_text_hash;
static size_t g_text_n, g_text_cap;
static uint32_t *g_text_idx;
static size_t g_text_idx_cap;
static int g_dirty;
static char g_dir[4096];
static uint64_t g_writes;
static int g_frame;

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void bin_rebuild(size_t cap) {
    uint32_t *idx = calloc(cap, sizeof *idx);
    if (!idx) rt_fatal("ledger: out of memory");
    for (size_t i = 0; i < g_bin_n; i++) {
        size_t j = (size_t)g_bin[i].hash & (cap - 1);
        while (idx[j]) j = (j + 1) & (cap - 1);
        idx[j] = (uint32_t)i + 1;
    }
    free(g_bin_idx);
    g_bin_idx = idx, g_bin_idx_cap = cap;
}

// 1 when the row was seen in this run; otherwise it is remembered.
static int bin_seen_or_add(const RowBin *r, uint64_t h) {
    if (g_bin_idx_cap) {
        for (size_t j = (size_t)h & (g_bin_idx_cap - 1); g_bin_idx[j]; j = (j + 1) & (g_bin_idx_cap - 1)) {
            const BinEntry *e = &g_bin[g_bin_idx[j] - 1];
            if (e->hash == h && !memcmp(&e->row, r, sizeof *r)) return 1;
        }
    }
    if (g_bin_n == g_bin_cap) {
        size_t cap = g_bin_cap ? 2 * g_bin_cap : 256;
        BinEntry *n = realloc(g_bin, cap * sizeof *n);
        if (!n) rt_fatal("ledger: out of memory for %zu rows", cap);
        g_bin = n, g_bin_cap = cap;
    }
    g_bin[g_bin_n].row = *r, g_bin[g_bin_n].hash = h;
    g_bin_n++;
    if (g_bin_n * 2 > g_bin_idx_cap) bin_rebuild(g_bin_idx_cap ? 2 * g_bin_idx_cap : 1024);
    else {
        size_t j = (size_t)h & (g_bin_idx_cap - 1);
        while (g_bin_idx[j]) j = (j + 1) & (g_bin_idx_cap - 1);
        g_bin_idx[j] = (uint32_t)g_bin_n;
    }
    return 0;
}

static void text_rebuild(size_t cap) {
    uint32_t *idx = calloc(cap, sizeof *idx);
    if (!idx) rt_fatal("ledger: out of memory");
    for (size_t i = 0; i < g_text_n; i++) {
        size_t j = (size_t)g_text_hash[i] & (cap - 1);
        while (idx[j]) j = (j + 1) & (cap - 1);
        idx[j] = (uint32_t)i + 1;
    }
    free(g_text_idx);
    g_text_idx = idx, g_text_idx_cap = cap;
}

// Takes ownership of line; 1 when it is new.
static int text_add(char *line) {
    size_t len = strlen(line);
    uint64_t h = fnv(line, len);
    if (g_text_idx_cap) {
        for (size_t j = (size_t)h & (g_text_idx_cap - 1); g_text_idx[j]; j = (j + 1) & (g_text_idx_cap - 1)) {
            size_t i = g_text_idx[j] - 1;
            if (g_text_hash[i] == h && !strcmp(g_text[i], line)) {
                free(line);
                return 0;
            }
        }
    }
    if (g_text_n == g_text_cap) {
        size_t cap = g_text_cap ? 2 * g_text_cap : 256;
        char **t = realloc(g_text, cap * sizeof *t);
        uint64_t *th = realloc(g_text_hash, cap * sizeof *th);
        if (!t || !th) rt_fatal("ledger: out of memory for %zu rows", cap);
        g_text = t, g_text_hash = th, g_text_cap = cap;
    }
    g_text[g_text_n] = line, g_text_hash[g_text_n] = h;
    g_text_n++;
    if (g_text_n * 2 > g_text_idx_cap) {
        text_rebuild(g_text_idx_cap ? 2 * g_text_idx_cap : 1024);
    } else {
        size_t j = (size_t)h & (g_text_idx_cap - 1);
        while (g_text_idx[j]) j = (j + 1) & (g_text_idx_cap - 1);
        g_text_idx[j] = (uint32_t)g_text_n;
    }
    return 1;
}

#ifdef OOT2_BENCH_TIMERS
static void add_row_body(const RowBin *r);
static void add_row(const RowBin *r) { BT_TIME(BT_U_LEDGER_ROW, add_row_body(r)); }
static void add_row_body(const RowBin *r) {
#else
static void add_row(const RowBin *r) {
#endif
    uint64_t h = fnv(r, sizeof *r);
    plat_mutex_lock(&g_lock);
    if (!bin_seen_or_add(r, h) && text_add(format_row(r))) g_dirty = 1;
    plat_mutex_unlock(&g_lock);
}

// ---------- the tracker
void ledger_track_reset(LedgerTrack *t) {
    memset(t, 0, sizeof *t);
    t->depth_fmt = -1;
}

static int tracker_slot(int kind) {
    switch (kind) {
    case NVN_STAGE_VS: return 0;
    case NVN_STAGE_PS: return 1;
    case NVN_STAGE_CS: return 2;
    case NVN_STAGE_TS: return 3;
    case NVN_STAGE_MS: return 4;
    default: return -1;
    }
}

static void invalidate_answers_digest(LedgerTrack *t, int stage) {
    for (int i = 0; i < 5; i++)
        if (KIND_STAGE[i] == stage) {
            t->answers_digest_valid &= (uint8_t)~(1u << i);
            return;
        }
}

static int view_format(const nvn_tex_rec *r) { return r->has_view && r->view.set_format ? r->view.format : r->tex.d.format; }

static void bind_program(LedgerTrack *t, const p_bind_program *p) {
    for (int s = 0; s < LEDGER_STAGES; s++) {
        int applies = (p->stages >> s) & 1;
        // Task and mesh stages follow their own bits, and any graphics bit: which bits NVN2's mask gives them was not
        // read from the driver, and a graphics program bound with all graphics bits must unbind them.
        if ((s == 6 || s == 7) && (p->stages & 0x1F)) applies = 1;
        if (!applies) continue;
        t->prog[s] = NULL;
        for (int i = 0; i < p->nstage && i < 6; i++)
            if (p->id[i] && KIND_STAGE[p->id[i]->kind] == s) t->prog[s] = p->id[i];
    }
}

// The counters of a texture bind (oot2/replay_count.h): the handle last bound at each slot, by the thread that binds (the
// tracker keeps only the type, and a null back end's tracker and the replay's run on different threads).
static _Thread_local uint64_t t_prev_handle[LEDGER_STAGES][LEDGER_TEX_SLOTS];

static void tex_count(const LedgerTrack *t, int stage, int slot, uint64_t handle, uint8_t type, int *all_same) {
    rc_add_(RC_TEX_BINDS, 1);
    if (t_prev_handle[stage][slot] == handle) rc_add_(RC_TEX_SAME_HANDLE, 1);
    t_prev_handle[stage][slot] = handle;
    if (t->tex[stage][slot] == type) rc_add_(RC_TEX_SAME_TYPE, 1);
    else *all_same = 0;
}

static void bind_tex(LedgerTrack *t, const p_bind_handles *b, int image) {
    if (b->stage < 0 || b->stage >= LEDGER_STAGES) return;
    invalidate_answers_digest(t, b->stage);
    int all_same = 1;
    for (int i = 0; i < b->n && i < 32; i++) {
        int slot = b->first + i;
        uint8_t type = 0;
        uint16_t fmt = 0;
        if (!desc_find(t->tex_pool, nvn_handle_texture_id(b->h[i]), &type, &fmt)) type = 0;
        if (image) {
            if (slot < 0 || slot >= LEDGER_IMG_SLOTS) continue;
            t->img_type[b->stage][slot] = type, t->img_fmt[b->stage][slot] = fmt;
        } else {
            // Index 32 to 39 is the image table's range: the translator answers it from "img" only, so a texture
            // bound there is never a "tex" answer
            if (slot < 0 || slot >= LEDGER_TEX_SLOTS || (slot >= 32 && slot < 40)) continue;
            if (RC_ON()) tex_count(t, b->stage, slot, b->h[i], type, &all_same);
            t->tex[b->stage][slot] = type;
        }
    }
    if (!image && RC_ON()) {
        rc_add_(RC_TEX_OPS, 1);
        if (all_same) rc_add_(RC_TEX_OPS_SAME_TYPE, 1);
    }
}

static void set_rt(LedgerTrack *t, const p_set_rt *p) {
    invalidate_answers_digest(t, 1);
    memset(t->fout, 0, sizeof t->fout);
    memset(t->color_fmt, 0, sizeof t->color_fmt);
    t->ncolor = 0, t->samples = 1;
    int n = p->n < 0 ? 0 : p->n > LEDGER_TARGETS ? LEDGER_TARGETS : p->n;
    for (int i = 0; i < n; i++) {
        const nvn_tex_rec *c = &p->color[i];
        if (!c->present) continue;
        t->color_fmt[i] = (uint16_t)view_format(c);
        t->fout[i] = class_of_format(view_format(c));
        t->ncolor = (uint8_t)(i + 1);
        if (c->tex.d.samples > 1 && t->samples == 1) t->samples = (uint8_t)c->tex.d.samples;
    }
    t->depth_fmt = p->depth.present ? view_format(&p->depth) : -1;
    if (p->depth.present && p->depth.tex.d.samples > 1 && t->samples == 1) t->samples = (uint8_t)p->depth.tex.d.samples;
}

static void bind_vattr(LedgerTrack *t, const p_bind_vattr *p) {
    invalidate_answers_digest(t, 0);
    for (int i = 0; i < p->n && i < NVN_VERTEX_BINDINGS; i++) {
        int loc = p->first + i;
        if (loc < 0 || loc >= LEDGER_ATTRS) continue;
        va_st a;
        memcpy(&a, &p->st[i], sizeof a);
        t->attr[loc] = a.fmt == NVN_FORMAT_NONE ? A_NONE : class_of_format((int)a.fmt);
    }
}

// The row of the stage with NVN stage number s, if one is bound.
static void emit(const LedgerTrack *t, int s, int raster) {
    const nvn_stage *st = t->prog[s];
    if (!st) return;
    RowBin r;
    memset(&r, 0, sizeof r);
    r.st = st;
    memcpy(r.tex, t->tex[s], sizeof r.tex);
    memcpy(r.img_type, t->img_type[s], sizeof r.img_type);
    memcpy(r.img_fmt, t->img_fmt[s], sizeof r.img_fmt);
    if (s == 0) memcpy(r.attr, t->attr, sizeof r.attr);
    if (s == 1) memcpy(r.fout, t->fout, sizeof r.fout);
    r.depth_fmt = -1;
    if (raster) {
        r.raster = 1;
        memcpy(r.color_fmt, t->color_fmt, sizeof r.color_fmt);
        r.depth_fmt = t->depth_fmt;
        r.ncolor = t->ncolor;
        r.samples = t->samples ? t->samples : 1;
    }

    add_row(&r);
}

// The counters of the answers digest (oot2/replay_count.h): by stage (vertex, pixel, any other), and what the last
// recompute of the thread for that stage had: its inputs and its value. The inputs of two recomputes are equal exactly when
// their values are (the digest is a function of them), which the counters check.
static _Thread_local struct {
    int have;
    RowBin in;
    uint8_t digest[32];
} t_prev_digest[5];

static int digest_kind(int slot) { return slot < 2 ? slot : 2; }

static void digest_count_recompute(int slot, const RowBin *r, const uint8_t out[32]) {
    int k = digest_kind(slot);
    rc_add_(RC_DG_RECOMP_VS + k, 1);
    if (t_prev_digest[slot].have) {
        if (!memcmp(&t_prev_digest[slot].in, r, sizeof *r)) rc_add_(RC_DG_INSAME_VS + k, 1);
        if (!memcmp(t_prev_digest[slot].digest, out, 32)) rc_add_(RC_DG_VALSAME_VS + k, 1);
    }
    t_prev_digest[slot].in = *r;
    memcpy(t_prev_digest[slot].digest, out, 32);
    t_prev_digest[slot].have = 1;
}

void ledger_track_answers_digest(LedgerTrack *t, int kind, uint8_t out[32]) {
    int slot = tracker_slot(kind);
    if (slot < 0) {
        memset(out, 0, 32);
        return;
    }
    RC_ADD(RC_DG_CALLS_VS + digest_kind(slot), 1);
    if (t->answers_digest_valid & (1u << slot)) {
        RC_ADD(RC_DG_HIT_VS + digest_kind(slot), 1);
        memcpy(out, t->answers_digest[slot], 32);
        return;
    }
    RowBin r;
    memset(&r, 0, sizeof r);
    int s = KIND_STAGE[slot];
    memcpy(r.tex, t->tex[s], sizeof r.tex);
    memcpy(r.img_type, t->img_type[s], sizeof r.img_type);
    memcpy(r.img_fmt, t->img_fmt[s], sizeof r.img_fmt);
    if (s == 0) memcpy(r.attr, t->attr, sizeof r.attr);
    if (s == 1) memcpy(r.fout, t->fout, sizeof r.fout);
    // A draw's state repeats the state of a draw before it far more often than it is new: the digest of the states seen
    // is kept by thread (the tracker of the front end and the replay's run on different threads), so the text and the
    // SHA-256 are made once for each state, not for each draw.
    enum { DCACHE = 2048 };
    typedef struct { RowBin r; uint8_t digest[32]; int used; } DEnt;
    static _Thread_local DEnt *t_dcache;
    if (!t_dcache) t_dcache = calloc(DCACHE, sizeof *t_dcache);
    DEnt *ce = NULL;
    if (t_dcache) {
        ce = &t_dcache[fnv(&r, sizeof r) % DCACHE];
        if (ce->used && !memcmp(&ce->r, &r, sizeof r)) {
            memcpy(out, ce->digest, 32);
            if (RC_ON()) digest_count_recompute(slot, &r, out);
            memcpy(t->answers_digest[slot], out, 32);
            t->answers_digest_valid |= (uint8_t)(1u << slot);
            return;
        }
    }
    BT_TIME(BT_S1_TEXT, char *text = answers_text(&r, out); free(text));
    if (ce) ce->r = r, memcpy(ce->digest, out, 32), ce->used = 1;
    if (RC_ON()) digest_count_recompute(slot, &r, out);
    memcpy(t->answers_digest[slot], out, 32);
    t->answers_digest_valid |= (uint8_t)(1u << slot);
}

// ---------- the projection (see oot2/ledger.h)
// A small reader of the description's JSON: only the few members the reads come from, everything else skipped.
typedef struct { const char *p, *e; } Jp;

static void jws(Jp *j) {
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\n' || *j->p == '\t' || *j->p == '\r')) j->p++;
}

static int jstr(Jp *j, char *out, size_t cap) {   // a string into out (cut to cap - 1), 1 on success
    jws(j);
    if (j->p >= j->e || *j->p != '"') return 0;
    j->p++;
    size_t n = 0;
    while (j->p < j->e && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->e) {
            c = *j->p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') {   // \uXXXX: the descriptions are ASCII; skip the four digits
                for (int k = 0; k < 4 && j->p < j->e; k++) j->p++;
                c = '?';
            }
        }
        if (out && n + 1 < cap) out[n++] = c;
    }
    if (j->p >= j->e) return 0;
    j->p++;
    if (out) out[n] = 0;
    return 1;
}

static int jskip(Jp *j);

static int jnum(Jp *j, long *v) {   // an integer, 1 on success
    jws(j);
    const char *q = j->p;
    int neg = 0;
    if (q < j->e && *q == '-') neg = 1, q++;
    if (q >= j->e || *q < '0' || *q > '9') return 0;
    long x = 0;
    while (q < j->e && *q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
    if (q < j->e && (*q == '.' || *q == 'e' || *q == 'E')) return 0;   // not an integer
    j->p = q;
    *v = neg ? -x : x;
    return 1;
}

static int jskip(Jp *j) {   // any one value
    jws(j);
    if (j->p >= j->e) return 0;
    char c = *j->p;
    if (c == '"') return jstr(j, NULL, 0);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        jws(j);
        if (j->p < j->e && *j->p == close) return j->p++, 1;
        for (;;) {
            if (c == '{') {
                if (!jstr(j, NULL, 0)) return 0;
                jws(j);
                if (j->p >= j->e || *j->p++ != ':') return 0;
            }
            if (!jskip(j)) return 0;
            jws(j);
            if (j->p >= j->e) return 0;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == close) return j->p++, 1;
            return 0;
        }
    }
    while (j->p < j->e && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\n') j->p++;   // a number, true, false, null
    return 1;
}

// The member loop of an object: calls cb for each key, with the reader after the colon; cb returns 1 when it consumed the value.
typedef int (*JMember)(void *u, const char *key, Jp *j);
static int jobject(Jp *j, JMember cb, void *u) {
    jws(j);
    if (j->p >= j->e || *j->p != '{') return 0;
    j->p++;
    jws(j);
    if (j->p < j->e && *j->p == '}') return j->p++, 1;
    for (;;) {
        char key[40];
        if (!jstr(j, key, sizeof key)) return 0;
        jws(j);
        if (j->p >= j->e || *j->p++ != ':') return 0;
        const char *before = j->p;
        if (!cb(u, key, j)) {
            j->p = before;
            if (!jskip(j)) return 0;
        }
        jws(j);
        if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; jws(j); continue; }
        if (*j->p == '}') return j->p++, 1;
        return 0;
    }
}

typedef struct {
    LedgerReads *r;
    int ps, dropped;
    // the member being read: a query or a vertex input or an output
    char kind[24], answer[40];
    long slot, islot, loc;
    int has_slot, has_islot, has_loc;
    int stage_known;
} RdCtx;

static uint8_t name_index(const char *const *names, int n, const char *answer) {   // 0xFF: no such name
    for (int i = 1; i < n; i++)
        if (!strcmp(names[i], answer)) return (uint8_t)i;
    return 0xFF;
}

// An answer for one slot: the first sets the default, a different second makes it "several" (0xFF, never equal)
static void note_def(uint8_t *read, uint8_t *def, uint8_t code) {
    if (!*read) *read = 1, *def = code;
    else if (*def != code) *def = 0xFF;
}

static int rd_query_member(void *u, const char *key, Jp *j) {
    RdCtx *c = u;
    if (!strcmp(key, "kind")) return jstr(j, c->kind, sizeof c->kind);
    if (!strcmp(key, "answer")) return jstr(j, c->answer, sizeof c->answer);
    if (!strcmp(key, "slot")) return c->has_slot = jnum(j, &c->slot);
    if (!strcmp(key, "image_slot")) return c->has_islot = jnum(j, &c->islot);
    return 0;
}

static int rd_read_queries(RdCtx *c, Jp *j) {
    jws(j);
    if (j->p >= j->e || *j->p != '[') return 0;
    j->p++;
    jws(j);
    if (j->p < j->e && *j->p == ']') return j->p++, 1;
    for (;;) {
        jws(j);
        c->kind[0] = c->answer[0] = 0, c->has_slot = c->has_islot = 0;
        if (j->p < j->e && *j->p == '{') {
            if (!jobject(j, rd_query_member, c)) return 0;
            if (c->has_slot && c->has_islot) {
                LedgerReads *r = c->r;
                if (!strcmp(c->kind, "sampler_type")) {
                    uint8_t code = name_index(TYPE_NAME, T_COUNT, c->answer);
                    if (c->islot >= 0 && c->islot < LEDGER_IMG_SLOTS) note_def(&r->img_type_read[c->islot], &r->img_type_def[c->islot], code);
                    else if (c->islot < 0 && c->slot >= 0 && c->slot < LEDGER_TEX_SLOTS) note_def(&r->tex_read[c->slot], &r->tex_def[c->slot], code);
                } else if (!strcmp(c->kind, "texture_format") && c->islot >= 0 && c->islot < LEDGER_IMG_SLOTS) {
                    int i = (int)c->islot;
                    if (!r->img_fmt_read[i]) r->img_fmt_read[i] = 1, snprintf(r->img_fmt_def[i], sizeof r->img_fmt_def[i], "%s", c->answer);
                    else if (strcmp(r->img_fmt_def[i], c->answer)) r->img_fmt_def[i][0] = 0;
                }
            }
        } else if (!jskip(j)) {
            return 0;
        }
        jws(j);
        if (j->p >= j->e) return 0;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') return j->p++, 1;
        return 0;
    }
}

static int rd_vin_member(void *u, const char *key, Jp *j) {
    RdCtx *c = u;
    if (!strcmp(key, "location")) return c->has_loc = jnum(j, &c->loc);
    if (!strcmp(key, "answer")) return jstr(j, c->answer, sizeof c->answer);
    return 0;
}

static int rd_out_member(void *u, const char *key, Jp *j) {
    RdCtx *c = u;
    if (!strcmp(key, "location")) return c->has_loc = jnum(j, &c->loc);
    return 0;
}

static int rd_iface_member(void *u, const char *key, Jp *j) {
    RdCtx *c = u;
    if (!strcmp(key, "outputs")) {
        jws(j);
        if (j->p >= j->e || *j->p != '[') return 0;
        j->p++;
        jws(j);
        if (j->p < j->e && *j->p == ']') return j->p++, 1;
        for (;;) {
            jws(j);
            c->has_loc = 0;
            if (j->p < j->e && *j->p == '{') {
                if (!jobject(j, rd_out_member, c)) return 0;
                if (c->has_loc && c->loc >= 0 && c->loc < LEDGER_TARGETS) c->r->fout_read |= 1u << c->loc;
            } else if (!jskip(j)) {
                return 0;
            }
            jws(j);
            if (j->p >= j->e) return 0;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') return j->p++, 1;
            return 0;
        }
    }
    return 0;
}

static int rd_top_member(void *u, const char *key, Jp *j) {
    RdCtx *c = u;
    char word[16];
    if (!strcmp(key, "stage")) {
        if (!jstr(j, word, sizeof word)) return 0;
        c->ps = !strcmp(word, "ps");
        return 1;
    }
    if (!strcmp(key, "queries_dropped")) {
        jws(j);
        c->dropped = j->p < j->e && *j->p == 't';
        return jskip(j);
    }
    if (!strcmp(key, "queries")) {
        jws(j);
        if (j->p < j->e && *j->p == 'n') return 0;   // null: nothing to read, skipped
        return rd_read_queries(c, j);
    }
    if (!strcmp(key, "vertex_inputs")) {
        jws(j);
        if (j->p >= j->e || *j->p != '[') return 0;
        j->p++;
        jws(j);
        if (j->p < j->e && *j->p == ']') return j->p++, 1;
        for (;;) {
            jws(j);
            c->has_loc = 0, c->answer[0] = 0;
            if (j->p < j->e && *j->p == '{') {
                if (!jobject(j, rd_vin_member, c)) return 0;
                if (c->has_loc && c->loc >= 0 && c->loc < LEDGER_ATTRS)
                    note_def(&c->r->attr_read[c->loc], &c->r->attr_def[c->loc], name_index(CLASS_NAME, 4, c->answer));
            } else if (!jskip(j)) {
                return 0;
            }
            jws(j);
            if (j->p >= j->e) return 0;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') return j->p++, 1;
            return 0;
        }
    }
    if (!strcmp(key, "interface")) {
        jws(j);
        if (j->p < j->e && *j->p == 'n') return 0;
        return jobject(j, rd_iface_member, c);
    }
    return 0;
}

int ledger_reads_parse(const char *desc, size_t n, LedgerReads *out) {
    memset(out, 0, sizeof *out);
    RdCtx c = {.r = out};
    Jp j = {desc, desc + n};
    if (!jobject(&j, rd_top_member, &c)) return 0;
    if (c.dropped) return 0;   // the record overflowed: prepare reads the declared slots instead, which is not done here
    // the pixel module's outputs are read only by a pixel module
    if (!c.ps) out->fout_read = 0;
    out->valid = 1;
    return 1;
}

int ledger_track_projected_digest(LedgerTrack *t, int kind, const LedgerReads *rd, uint8_t out[32], char **text_out) {
    memset(out, 0, 32);
    if (text_out) *text_out = NULL;
    int slot = tracker_slot(kind);
    if (slot < 0 || !rd->valid) return 0;
    int s = KIND_STAGE[slot];
    RowBin r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < LEDGER_TEX_SLOTS; i++) {
        uint8_t v = t->tex[s][i];
        if (v && rd->tex_read[i] && rd->tex_def[i] != v) r.tex[i] = v;
    }
    for (int i = 0; i < LEDGER_IMG_SLOTS; i++) {
        uint8_t v = t->img_type[s][i];
        if (!v) continue;
        int type_differs = rd->img_type_read[i] && rd->img_type_def[i] != v;
        int fmt_differs = rd->img_fmt_read[i] && strcmp(rd->img_fmt_def[i], image_format_name(t->img_fmt[s][i]));
        if (type_differs || fmt_differs) r.img_type[i] = v, r.img_fmt[i] = t->img_fmt[s][i];
    }
    if (s == 0)
        for (int i = 0; i < LEDGER_ATTRS; i++) {
            uint8_t v = t->attr[i];
            if (v && rd->attr_read[i] && rd->attr_def[i] != v) r.attr[i] = v;
        }
    if (s == 1)
        for (int i = 0; i < LEDGER_TARGETS; i++) {
            uint8_t v = t->fout[i];
            if (v && (rd->fout_read >> i & 1) && v != A_FLOAT) r.fout[i] = v;
        }
    char *text = answers_text(&r, out);
    int any = 0;
    for (int i = 0; i < 32; i++) any |= out[i] != 0;
    if (any && text_out) *text_out = text;
    else free(text);
    return any;
}

void ledger_track_op(LedgerTrack *t, const op_hdr *h) {
    const void *a = h + 1;
    switch (h->op) {
    case OP_SET_TEX_POOL: t->tex_pool = ((const p_set_pool *)a)->pool.addr; break;
    case OP_BIND_PROGRAM: bind_program(t, a); break;
    case OP_BIND_TEX: bind_tex(t, a, 0); break;
    case OP_BIND_IMG: bind_tex(t, a, 1); break;
    case OP_BIND_VATTR: bind_vattr(t, a); break;
    case OP_SET_RT: set_rt(t, a); break;
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND:
        emit(t, 0, 1);
        emit(t, 1, 1);
        break;
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
        emit(t, 6, 1);
        emit(t, 7, 1);
        emit(t, 1, 1);
        break;
    case OP_DISPATCH: case OP_DISPATCH_IND: emit(t, 5, 0); break;
    default: break;
    }
}

// ---------- collection (settings.h collect_variants)
// The draws a development run skipped because they stopped on an answer the ledger can fix, and the distinct (stage,
// answer) pairs among them (a set of 64-bit hashes: a pair seen twice is the same stop, logged once). Its own lock, so a
// note never waits for the rows' lock.
static PlatMutex g_clock = PLAT_MUTEX_INIT;
static uint64_t *g_cset;
static size_t g_cset_n, g_cset_cap;
static uint64_t g_collected;

uint64_t ledger_collect_note(const char *stage, const char *answer, int *first) {
    uint64_t h = fnv(stage, strlen(stage));
    h = (h ^ 0xFF) * 1099511628211ull;   // a separator no text holds, so ("ab", "c") and ("a", "bc") differ
    for (const char *c = answer; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    int isnew = 1;
    plat_mutex_lock(&g_clock);
    for (size_t i = 0; i < g_cset_n; i++)
        if (g_cset[i] == h) {
            isnew = 0;
            break;
        }
    if (isnew) {
        if (g_cset_n == g_cset_cap) {
            size_t cap = g_cset_cap ? g_cset_cap * 2 : 64;
            uint64_t *grown = realloc(g_cset, cap * sizeof *grown);
            if (!grown) rt_fatal("ledger: out of memory for the collected stops");
            g_cset = grown, g_cset_cap = cap;
        }
        g_cset[g_cset_n++] = h;
    }
    uint64_t n = ++g_collected;
    plat_mutex_unlock(&g_clock);
    if (first) *first = isnew;
    return n;
}

uint64_t ledger_collected(void) {
    plat_mutex_lock(&g_clock);
    uint64_t n = g_collected;
    plat_mutex_unlock(&g_clock);
    return n;
}

uint64_t ledger_collected_kinds(void) {
    plat_mutex_lock(&g_clock);
    uint64_t n = g_cset_n;
    plat_mutex_unlock(&g_clock);
    return n;
}

// ---------- the file
static int flush_file(int stopping);
// A stop ends the process, so a flush that waits for a safe point has no later one: from the stop path the file is written
// even from inside a submission (a replay that stops on a draw is inside one, and the answers it stopped on are what the
// next prepare needs). A thread that holds the rows is not waited for: a stop must end.
static void stop_flush(void) { flush_file(1); }
void ledger_flush_on_stop(void) { rt_set_stop_flush(stop_flush); }

void ledger_frame_enter(void) { __atomic_fetch_add(&g_frame, 1, __ATOMIC_ACQ_REL); }
void ledger_frame_leave(void) { __atomic_fetch_sub(&g_frame, 1, __ATOMIC_ACQ_REL); }

static void path_of(char *out, size_t size, const char *suffix) {
    snprintf(out, size, "%s/%s%s", g_dir, LEDGER_FILE, suffix);
}

// Rows already in the folder's file join the set: a run adds to what earlier runs met. g_lock held.
static void load_file(void) {
    char path[4200];
    path_of(path, sizeof path, "");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    int c;
    size_t n = 0;
    uint64_t kept = 0;
    do {
        c = fgetc(f);
        if (c == '\n' || c == EOF) {
            if (n && n < (1u << 20) && !strncmp(line, "{\"key\":\"", 8)) {
                char *copy = malloc(n + 1);
                if (!copy) rt_fatal("ledger: out of memory");
                memcpy(copy, line, n);
                copy[n] = 0;
                if (n && copy[n - 1] == '\r') copy[n - 1] = 0;
                kept += (uint64_t)text_add(copy);
            }
            n = 0;
            continue;
        }
        if (n + 1 >= cap) {
            cap = cap ? 2 * cap : 4096;
            line = realloc(line, cap);
            if (!line) rt_fatal("ledger: out of memory");
        }
        line[n++] = (char)c;
    } while (c != EOF);
    free(line);
    fclose(f);
    rt_log("ledger: %llu rows read from %s", (ull)kept, path);
}

void ledger_set_folder(const char *dir) {
    plat_mutex_lock(&g_lock);
    snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
    if (g_dir[0]) load_file();
    plat_mutex_unlock(&g_lock);
}

static int cmp_text(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int replace_file(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

int ledger_flush(void) { return flush_file(0); }

static int flush_file(int stopping) {
    if (!stopping && __atomic_load_n(&g_frame, __ATOMIC_ACQUIRE) > 0) return -1;
    if (stopping) {
        if (!plat_mutex_trylock(&g_lock)) return -1;
    } else {
        plat_mutex_lock(&g_lock);
    }
    int result = 0;
    if (g_dir[0] && g_dirty) {
        char **rows = malloc((g_text_n ? g_text_n : 1) * sizeof *rows);
        if (!rows) rt_fatal("ledger: out of memory");
        memcpy(rows, g_text, g_text_n * sizeof *rows);
        qsort(rows, g_text_n, sizeof *rows, cmp_text);
        char path[4200], tmp[4200];
        path_of(path, sizeof path, "");
        path_of(tmp, sizeof tmp, ".tmp");
        FILE *f = fopen(tmp, "wb");
        int ok = f != NULL;
        if (ok) ok = fputs("{\"oot2_ledger\":1}\n", f) >= 0;
        for (size_t i = 0; ok && i < g_text_n; i++) ok = fputs(rows[i], f) >= 0 && fputc('\n', f) >= 0;
        if (f) ok = (fclose(f) == 0) && ok;
        if (ok) ok = replace_file(tmp, path);
        free(rows);
        if (ok) {
            g_dirty = 0, g_writes++, result = 1;
            rt_log("ledger: %zu rows written to %s", g_text_n, path);
        } else {
            remove(tmp);
            rt_log("ledger: cannot write %s", path);
            result = -1;
        }
    }
    plat_mutex_unlock(&g_lock);
    return result;
}

uint64_t ledger_rows(void) {
    plat_mutex_lock(&g_lock);
    uint64_t n = g_text_n;
    plat_mutex_unlock(&g_lock);
    return n;
}

const char *ledger_row_text(uint64_t i) {
    plat_mutex_lock(&g_lock);
    const char *s = i < g_text_n ? g_text[i] : NULL;
    plat_mutex_unlock(&g_lock);
    return s;
}

uint64_t ledger_writes(void) {
    plat_mutex_lock(&g_lock);
    uint64_t n = g_writes;
    plat_mutex_unlock(&g_lock);
    return n;
}

void ledger_reset(void) {
    plat_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_text_n; i++) free(g_text[i]);
    free(g_text), free(g_text_hash), free(g_text_idx), free(g_bin), free(g_bin_idx);
    g_text = NULL, g_text_hash = NULL, g_text_idx = NULL, g_bin = NULL, g_bin_idx = NULL;
    g_text_n = g_text_cap = g_text_idx_cap = g_bin_n = g_bin_cap = g_bin_idx_cap = 0;
    g_dirty = 0, g_dir[0] = 0, g_writes = 0;
    plat_mutex_unlock(&g_lock);
    plat_mutex_lock(&g_clock);
    free(g_cset);
    g_cset = NULL, g_cset_n = g_cset_cap = 0, g_collected = 0;
    plat_mutex_unlock(&g_clock);
}
