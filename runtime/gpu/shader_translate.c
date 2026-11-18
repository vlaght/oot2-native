// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The oot2-native contributors
//
// Translation of a shader variant on demand (oot2/shader_translate.h): the replay asks for a module under answers that the
// packs do not hold, and the translator (oot2-xlate --serve, the same child prepare runs; its line protocol is in
// tools/shaders/pool.py) makes it from the job record of the program, in a process that stays warm between requests. The
// variants come back whole in the reply line and stay in memory for the run; a run that made some wrote their ledger rows
// as it went, so prepare packs them and the next run finds them in the pack.
//
// The job is the program's record from shaders/split/jobs.jsonl, its code path pointing at programs.bin where it is, with
// the answers the module reads added as its top-level members (tools/shaders/corpus.py variant_job). The translator is
// found by OOT2_XLATE, else oot2-xlate in the shaders folder's parent (the game folder), else on PATH.
//
// POSIX only for now (fork, pipes): on another platform nothing is available and every request is refused, which leaves
// the replay as it was (a state the packs cannot serve stops by name).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oot2/runtime.h"
#include "oot2/shader_cache.h"
#include "oot2/shader_translate.h"
#include "plat.h"

#if defined(_WIN32)

void sht_open(const char *base_pack_path) { (void)base_pack_path; }
int sht_variant(const char *key, const uint8_t digest[32], const char *answers, int *index, char *why, size_t cap) {
    (void)key, (void)digest, (void)answers, (void)index;
    snprintf(why, cap, "translation on demand is not built for this platform");
    return 0;
}
int sht_entry(int index, ShcEntry *out) { (void)index, (void)out; return 0; }
uint64_t sht_count(void) { return 0; }

#else

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

typedef struct {
    char key[SHC_KEY_BYTES];
    uint8_t digest[SHC_DIGEST_BYTES];
    uint8_t *module, *desc;       // the module's bytes and the description (or the failure reason)
    uint32_t module_bytes, desc_bytes, status;
} Var;

typedef struct { uint32_t off, len; } Line;

static struct {
    PlatMutex mu;
    char shaders[1024];           // the folder of the packs
    int opened;
    // the job records
    char *jobs;                   // jobs.jsonl, read whole when first needed
    size_t jobs_n;
    Line *line;                   // open addressing over the keys: line[i].len 0 is empty
    uint32_t line_cap;
    int jobs_tried;
    // the child
    pid_t pid;
    int to, from;
    int spawn_tried;
    // the variants made
    Var *v;
    int nv, cap;
    uint64_t made_us;
} T = {.mu = PLAT_MUTEX_INIT, .to = -1, .from = -1};

void sht_open(const char *base_pack_path) {
    plat_mutex_lock(&T.mu);
    T.opened = 0;
    const char *slash = strrchr(base_pack_path, '/');
    if (slash && (size_t)(slash - base_pack_path) < sizeof T.shaders) {
        memcpy(T.shaders, base_pack_path, (size_t)(slash - base_pack_path));
        T.shaders[slash - base_pack_path] = 0;
        T.opened = 1;
    }
    plat_mutex_unlock(&T.mu);
}

uint64_t sht_count(void) { return (uint64_t)T.nv; }

static uint32_t key_hash(const char *k, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)k[i]) * 16777619u;
    return h;
}

// jobs.jsonl whole, and an index of its lines by key; every line starts {"key":"<key>"
static int jobs_load(char *why, size_t cap) {
    if (T.jobs) return 1;
    if (T.jobs_tried) return 0;
    T.jobs_tried = 1;
    char path[1200];
    snprintf(path, sizeof path, "%s/split/jobs.jsonl", T.shaders);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, cap, "the job records %s cannot be opened", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        snprintf(why, cap, "the job records %s cannot be read", path);
        return 0;
    }
    fclose(f);
    buf[n] = 0;
    uint32_t lines = 0;
    for (long i = 0; i < n; i++) lines += buf[i] == '\n';
    uint32_t capi = 1024;
    while (capi < lines * 2 + 16) capi *= 2;
    Line *idx = calloc(capi, sizeof *idx);
    if (!idx) {
        free(buf);
        snprintf(why, cap, "no memory for the job index");
        return 0;
    }
    long at = 0;
    while (at < n) {
        char *nl = memchr(buf + at, '\n', (size_t)(n - at));
        size_t len = nl ? (size_t)(nl - (buf + at)) : (size_t)(n - at);
        if (len > 12 && !strncmp(buf + at, "{\"key\":\"", 8)) {
            const char *k = buf + at + 8;
            const char *q = memchr(k, '"', len - 8);
            if (q && q - k < (long)SHC_KEY_BYTES) {
                uint32_t h = key_hash(k, (size_t)(q - k)) & (capi - 1);
                while (idx[h].len) h = (h + 1) & (capi - 1);
                idx[h] = (Line){(uint32_t)at, (uint32_t)len};
            }
        }
        at += (long)len + 1;
    }
    T.jobs = buf, T.jobs_n = (size_t)n, T.line = idx, T.line_cap = capi;
    return 1;
}

static int job_find(const char *key, const char **line, size_t *len) {
    size_t kn = strlen(key);
    uint32_t h = key_hash(key, kn) & (T.line_cap - 1);
    while (T.line[h].len) {
        const char *l = T.jobs + T.line[h].off;
        if (!strncmp(l + 8, key, kn) && l[8 + kn] == '"') {
            *line = l, *len = T.line[h].len;
            return 1;
        }
        h = (h + 1) & (T.line_cap - 1);
    }
    return 0;
}

static void child_close(void) {
    if (T.to >= 0) close(T.to);
    if (T.from >= 0) close(T.from);
    T.to = T.from = -1;
    if (T.pid > 0) {
        kill(T.pid, SIGTERM);
        waitpid(T.pid, NULL, 0);
    }
    T.pid = 0;
}

static int child_spawn(char *why, size_t cap) {
    if (T.pid > 0) return 1;
    if (T.spawn_tried >= 3) return 0;   // a translator that will not start is not asked again and again
    T.spawn_tried++;
    char exe[1300];
    const char *env = getenv("OOT2_XLATE");
    if (env && *env) {
        snprintf(exe, sizeof exe, "%s", env);
    } else {
        snprintf(exe, sizeof exe, "%s/../oot2-xlate", T.shaders);   // the game folder
        if (access(exe, X_OK)) snprintf(exe, sizeof exe, "oot2-xlate");   // else on PATH
    }
    int in[2], out[2];
    if (pipe(in) || pipe(out)) {
        snprintf(why, cap, "pipes for the translator: %s", strerror(errno));
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);   // a translator that died is seen as a failed write, not a signal
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(why, cap, "fork for the translator: %s", strerror(errno));
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        return 0;
    }
    if (!pid) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        dup2(in[0], 0);
        dup2(out[1], 1);
        char log[1300];
        snprintf(log, sizeof log, "%s/xlate-runtime.log", T.shaders);
        int lg = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lg >= 0) dup2(lg, 2);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        execlp(exe, exe, "--serve", (char *)NULL);
        _exit(127);
    }
    close(in[0]), close(out[1]);
    T.pid = pid, T.to = in[1], T.from = out[0];
    rt_log("gfx: vulkan: shader translation on demand: started %s (process %d)", exe, (int)pid);
    return 1;
}

// One reply line into a growing buffer (without the newline); 1 on a whole line
static int read_line(char **buf, size_t *n, size_t *cap) {
    *n = 0;
    for (;;) {
        if (*n + 2 > *cap) {
            *cap = *cap ? *cap * 2 : 1u << 16;
            char *g = realloc(*buf, *cap);
            if (!g) return 0;
            *buf = g;
        }
        char c;
        ssize_t r = read(T.from, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        if (c == '\n') {
            (*buf)[*n] = 0;
            return 1;
        }
        (*buf)[(*n)++] = c;
    }
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

// base64 text of n characters into fresh bytes; NULL on a bad character
static uint8_t *b64_decode(const char *s, size_t n, uint32_t *out_n) {
    uint8_t *o = malloc(n / 4 * 3 + 4);
    if (!o) return NULL;
    size_t k = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '=') break;
        int v = b64_val((unsigned char)s[i]);
        if (v < 0) {
            free(o);
            return NULL;
        }
        acc = acc << 6 | (uint32_t)v, bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o[k++] = (uint8_t)(acc >> bits);
        }
    }
    *out_n = (uint32_t)k;
    return o;
}

static int request(const char *job, size_t job_n, Var *out, char *why, size_t cap) {
    static char *reply;
    static size_t reply_cap;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!child_spawn(why, cap)) return 0;
        size_t sent = 0;
        const char head[] = "{\"job\":";
        struct { const char *p; size_t n; } parts[3] = {{head, sizeof head - 1}, {job, job_n}, {"}\n", 2}};
        int ok = 1;
        for (int i = 0; i < 3 && ok; i++)
            for (sent = 0; sent < parts[i].n;) {
                ssize_t w = write(T.to, parts[i].p + sent, parts[i].n - sent);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { ok = 0; break; }
                sent += (size_t)w;
            }
        size_t n = 0;
        if (ok && read_line(&reply, &n, &reply_cap)) {
            // ok <key> <microseconds> <module> <description>   |   fail <key> <microseconds> <message>
            char *p = reply;
            if (!strncmp(p, "ok ", 3)) {
                char *key = p + 3, *sp = strchr(key, ' ');
                char *us = sp ? sp + 1 : NULL, *sp2 = us ? strchr(us, ' ') : NULL;
                char *mod = sp2 ? sp2 + 1 : NULL, *sp3 = mod ? strchr(mod, ' ') : NULL;
                if (!sp3) {
                    snprintf(why, cap, "the translator's reply is not in the form the protocol has");
                    return 0;
                }
                if (us) T.made_us += (uint64_t)strtoull(us, NULL, 10);
                char *desc = sp3 + 1;
                out->module = b64_decode(mod, (size_t)(sp3 - mod), &out->module_bytes);
                out->desc = b64_decode(desc, strlen(desc), &out->desc_bytes);
                if (!out->module || !out->desc) {
                    free(out->module), free(out->desc);
                    snprintf(why, cap, "the translator's reply holds a bad base64 text");
                    return 0;
                }
                out->status = SHC_TRANSLATED;
                return 1;
            }
            snprintf(why, cap, "the translator refused the job: %.300s", reply);
            return 0;
        }
        child_close();   // it died or closed its pipe: once more with a fresh one
    }
    snprintf(why, cap, "the translator does not answer");
    return 0;
}

int sht_variant(const char *key, const uint8_t digest[32], const char *answers, int *index, char *why, size_t cap) {
    plat_mutex_lock(&T.mu);
    for (int i = 0; i < T.nv; i++)
        if (!strcmp(T.v[i].key, key) && !memcmp(T.v[i].digest, digest, SHC_DIGEST_BYTES)) {
            *index = i;
            plat_mutex_unlock(&T.mu);
            return 1;
        }
    int ok = 0;
    if (!T.opened) {
        snprintf(why, cap, "the shader packs are not open");
    } else if (jobs_load(why, cap)) {
        const char *line;
        size_t n;
        if (!job_find(key, &line, &n)) {
            snprintf(why, cap, "no job record of the program %s in the packs' split", key);
        } else {
            // the record with its code path where programs.bin is, and the answers as members
            const char *c = strstr(line, "\"code\":\"");
            const char *cend = c && (size_t)(c - line) < n ? strchr(c + 8, '"') : NULL;
            size_t alen = strlen(answers);
            if (!cend || alen < 2 || line[n - 1] != '}') {
                snprintf(why, cap, "the job record of %s is not in the form the translator's contract has", key);
            } else {
                char code[1300];
                snprintf(code, sizeof code, "%s/split/programs.bin", T.shaders);
                size_t cap_job = n + strlen(code) + alen + 16;
                char *job = malloc(cap_job);
                if (!job) {
                    snprintf(why, cap, "no memory for a job");
                } else {
                    size_t k = (size_t)(c - line) + 8;
                    memcpy(job, line, k);
                    k += (size_t)snprintf(job + k, cap_job - k, "%s", code);
                    size_t tail = n - 1 - (size_t)(cend - line);   // from the closing quote to before the final brace
                    memcpy(job + k, cend, tail);
                    k += tail;
                    job[k++] = ',';
                    memcpy(job + k, answers + 1, alen - 2);   // the answers' object without its braces
                    k += alen - 2;
                    job[k++] = '}';
                    Var v;
                    memset(&v, 0, sizeof v);
                    snprintf(v.key, sizeof v.key, "%s", key);
                    memcpy(v.digest, digest, SHC_DIGEST_BYTES);
                    ok = request(job, k, &v, why, cap);
                    free(job);
                    if (ok) {
                        if (T.nv == T.cap) {
                            int nc = T.cap ? T.cap * 2 : 64;
                            Var *g = realloc(T.v, sizeof *g * (size_t)nc);
                            if (!g) rt_fatal("gfx: vulkan: out of memory for the shader variants made on demand");
                            T.v = g, T.cap = nc;
                        }
                        T.v[T.nv] = v;
                        *index = T.nv++;
                    }
                }
            }
        }
    }
    plat_mutex_unlock(&T.mu);
    return ok;
}

int sht_entry(int index, ShcEntry *out) {
    plat_mutex_lock(&T.mu);
    int ok = index >= 0 && index < T.nv;
    if (ok) {
        const Var *v = &T.v[index];
        memset(out, 0, sizeof *out);
        out->key = v->key;
        out->answers = v->digest;
        out->status = v->status;
        out->module = (const uint32_t *)(const void *)v->module;
        out->module_bytes = v->module_bytes;
        out->description = (const char *)v->desc;
        out->description_bytes = v->desc_bytes;
    }
    plat_mutex_unlock(&T.mu);
    return ok;
}

#endif
