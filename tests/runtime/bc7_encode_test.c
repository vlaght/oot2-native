// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The oot2-native contributors
//
// The BC7 encoder on the device (runtime/gpu/shaders/bc7_encode.comp; the ASTC transcode). Driven by
// tests/test_runtime_vk_bc7.py:
//
//     bc7_encode_test <shader.spv>                       encode seeded images, decode the blocks here, check the error
//     bc7_encode_test <shader.spv> <in.rgba> <w> <h> <out.bc7>   encode one image (w*h RGBA8 texels) and write its blocks
//
// It makes its own instance and device (Vulkan 1.3, buffer device addresses and 64-bit integers) and links no runtime code.
// The first form builds images of different kinds (a gradient, a ramp with noise, a colour edge, a smooth blob, an alpha
// ramp, white noise), sizes that are not multiples of four among them, encodes each in one dispatch, decodes the blocks with
// the format's mode 6 as the specification says (the encoder writes no other mode) and prints the PSNR of each against its
// image. Every block must name mode 6, and each kind must keep its PSNR above its floor. Prints `ok bc7_encode` and exits 0;
// exit 1: a floor missed; exit 3: no Vulkan loader or device.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "volk.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef unsigned long long ull;
#define EXIT_NO_DEVICE 3
#define CHECK(c) do { if (!(c)) { printf("check failed at line %d: %s\n", __LINE__, #c); exit(2); } } while (0)


// ---------- a device of our own

typedef struct Dev {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties mem;
} Dev;

static Dev d;

static void device_or_exit(void) {
    if (volkInitialize() != VK_SUCCESS) {
        printf("no Vulkan loader\n");
        exit(EXIT_NO_DEVICE);
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&ci, NULL, &d.inst) != VK_SUCCESS) {
        printf("no Vulkan 1.3 instance\n");
        exit(EXIT_NO_DEVICE);
    }
    volkLoadInstance(d.inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d.inst, &n, NULL);
    if (!n) {
        printf("no Vulkan device\n");
        exit(EXIT_NO_DEVICE);
    }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(d.inst, &n, devs);
    for (uint32_t i = 0; i < n && !d.phys; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
        vkGetPhysicalDeviceFeatures2(devs[i], &f);
        if (!f12.bufferDeviceAddress || !f.features.shaderInt64) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties qp[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qp);
        for (uint32_t q = 0; q < qn; q++)
            if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                d.phys = devs[i], d.family = q;
                printf("device: %s\n", p.deviceName);
                break;
            }
    }
    if (!d.phys) {
        printf("no Vulkan 1.3 compute device with buffer device addresses and 64-bit integers\n");
        exit(EXIT_NO_DEVICE);
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = d.family,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .bufferDeviceAddress = VK_TRUE};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12,
                                   .features = {.shaderInt64 = VK_TRUE}};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(d.phys, &di, NULL, &d.dev) == VK_SUCCESS);
    volkLoadDevice(d.dev);
    vkGetDeviceQueue(d.dev, d.family, 0, &d.queue);
    vkGetPhysicalDeviceMemoryProperties(d.phys, &d.mem);
}

typedef struct Buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    uint64_t addr, size;
} Buf;

static Buf buf_new(uint64_t size) {
    Buf b = {.size = size};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    CHECK(vkCreateBuffer(d.dev, &bi, NULL, &b.buf) == VK_SUCCESS);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d.dev, b.buf, &mr);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < d.mem.memoryTypeCount; i++)
        if ((mr.memoryTypeBits >> i & 1) && (d.mem.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
            break;
        }
    CHECK(type != UINT32_MAX);
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = mr.size,
                               .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(d.dev, &alloc_info, NULL, &b.mem) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(d.dev, b.buf, b.mem, 0) == VK_SUCCESS);
    void *map;
    CHECK(vkMapMemory(d.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS);
    b.map = map;
    VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buf};
    b.addr = vkGetBufferDeviceAddress(d.dev, &da);
    return b;
}

static void buf_free(Buf *b) {
    vkDestroyBuffer(d.dev, b->buf, NULL);
    vkFreeMemory(d.dev, b->mem, NULL);
}

// ---------- the pass

typedef struct Push {
    uint64_t src, dst;
    uint32_t pitch, width, height, blocks_x, blocks_y, pad;
} Push;
_Static_assert(sizeof(Push) == 40, "the push constants of bc7_encode.comp are 40 bytes");

static VkPipeline g_pipe;
static VkPipelineLayout g_layout;
static VkCommandPool g_pool;
static VkCommandBuffer g_cb;
static VkFence g_fence;

static void pipeline_make(const char *spv_path) {
    FILE *f = fopen(spv_path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK(n > 0 && n % 4 == 0);
    uint32_t *code = malloc((size_t)n);
    CHECK(code && fread(code, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(d.dev, &mi, NULL, &m) == VK_SUCCESS);
    free(code);
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pr};
    CHECK(vkCreatePipelineLayout(d.dev, &li, NULL, &g_layout) == VK_SUCCESS);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = g_layout};
    CHECK(vkCreateComputePipelines(d.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipe) == VK_SUCCESS);
    vkDestroyShaderModule(d.dev, m, NULL);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = d.family};
    CHECK(vkCreateCommandPool(d.dev, &pi, NULL, &g_pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(d.dev, &alloc_info, &g_cb) == VK_SUCCESS);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(d.dev, &fi, NULL, &g_fence) == VK_SUCCESS);
}

static void run_pass(const Push *p) {
    CHECK(vkResetCommandBuffer(g_cb, 0) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(g_cb, &bi) == VK_SUCCESS);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdPushConstants(g_cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *p, p);
    vkCmdDispatch(g_cb, (p->blocks_x + 7) / 8, (p->blocks_y + 7) / 8, 1);
    CHECK(vkEndCommandBuffer(g_cb) == VK_SUCCESS);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_cb};
    CHECK(vkQueueSubmit(d.queue, 1, &si, g_fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(d.dev, 1, &g_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkResetFences(d.dev, 1, &g_fence) == VK_SUCCESS);
}


// ---------- BC7 mode 6, decoded as the specification reads it

static uint32_t bits_at(const uint8_t *b, uint32_t pos, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)((b[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
    return v;
}

static const uint8_t W4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// the 16 texels of one block, RGBA each; 0 when the block is not mode 6
static int decode_mode6(const uint8_t *b, uint8_t out[16][4]) {
    if ((b[0] & 0x7f) != 0x40) return 0;
    uint32_t pos = 7, e[2][4];
    for (int c = 0; c < 4; c++) {
        e[0][c] = bits_at(b, pos, 7), pos += 7;
        e[1][c] = bits_at(b, pos, 7), pos += 7;
    }
    uint32_t p0 = bits_at(b, pos++, 1), p1 = bits_at(b, pos++, 1);
    for (int c = 0; c < 4; c++) e[0][c] = e[0][c] << 1 | p0, e[1][c] = e[1][c] << 1 | p1;
    for (int i = 0; i < 16; i++) {
        uint32_t n = i == 0 ? 3 : 4, idx = bits_at(b, pos, n);
        pos += n;
        for (int c = 0; c < 4; c++) out[i][c] = (uint8_t)((e[0][c] * (64 - W4[idx]) + e[1][c] * W4[idx] + 32) >> 6);
    }
    return 1;
}

// ---------- the images

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { return g_rng = g_rng * 1664525u + 1013904223u; }

enum { K_GRADIENT, K_RAMP_NOISE, K_EDGE, K_BLOB, K_ALPHA, K_NOISE, K_KINDS };
static const char *const k_kind[K_KINDS] = {"gradient", "ramp+noise", "colour edge", "smooth blob", "alpha ramp", "white noise"};
// the lowest PSNR (dB) a kind may have: a smooth image is held to a high one, noise (which no 8-bit-per-texel code keeps) to a low one
static const double k_floor[K_KINDS] = {33.0, 34.0, 34.0, 44.0, 40.0, 13.0};

static void make_image(int kind, uint32_t w, uint32_t h, uint8_t *px) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = px + 4 * (y * w + x);
            float u = (float)x / (float)w, v = (float)y / (float)h;
            float r = 0, g = 0, b = 0, a = 255;
            switch (kind) {
            case K_GRADIENT: r = 255 * u, g = 255 * v, b = 128 + 100 * sinf(6.0f * u); break;
            case K_RAMP_NOISE: r = 255 * u + (float)(rnd() % 9) - 4, g = 200 * v + (float)(rnd() % 9) - 4, b = 90 + (float)(rnd() % 9); break;
            case K_EDGE: r = u + 0.3f * v > 0.6f ? 230 : 25, g = u + 0.3f * v > 0.6f ? 40 : 200, b = 120; break;
            case K_BLOB: {
                float dx = u - 0.5f, dy = v - 0.5f, d = expf(-12.0f * (dx * dx + dy * dy));
                r = 255 * d, g = 180 * d + 30, b = 60 + 120 * (1.0f - d);
                break;
            }
            case K_ALPHA: r = 200, g = 90 + 100 * u, b = 40, a = 255 * v; break;
            default: r = (float)(rnd() & 255), g = (float)(rnd() & 255), b = (float)(rnd() & 255), a = 255; break;
            }
            p[0] = (uint8_t)fminf(fmaxf(r, 0), 255), p[1] = (uint8_t)fminf(fmaxf(g, 0), 255);
            p[2] = (uint8_t)fminf(fmaxf(b, 0), 255), p[3] = (uint8_t)fminf(fmaxf(a, 0), 255);
        }
}

// encode w*h RGBA texels into blocks (blocks_x * blocks_y * 16 bytes into out)
static void encode(const uint8_t *px, uint32_t w, uint32_t h, uint8_t *out) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    Buf src = buf_new((uint64_t)w * h * 4), dst = buf_new((uint64_t)bx * by * 16);
    memcpy(src.map, px, (size_t)w * h * 4);
    memset(dst.map, 0, (size_t)bx * by * 16);
    Push p = {.src = src.addr, .dst = dst.addr, .pitch = w, .width = w, .height = h, .blocks_x = bx, .blocks_y = by};
    run_pass(&p);
    memcpy(out, dst.map, (size_t)bx * by * 16);
    buf_free(&src), buf_free(&dst);
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 6) {
        printf("usage: bc7_encode_test <shader.spv> [<in.rgba> <w> <h> <out.bc7>]\n");
        return 2;
    }
    device_or_exit();
    pipeline_make(argv[1]);
    if (argc == 6) {   // one image from a file
        uint32_t w = (uint32_t)atoi(argv[3]), h = (uint32_t)atoi(argv[4]);
        FILE *f = fopen(argv[2], "rb");
        CHECK(f && w && h);
        uint8_t *px = malloc((size_t)w * h * 4);
        CHECK(px && fread(px, 1, (size_t)w * h * 4, f) == (size_t)w * h * 4);
        fclose(f);
        uint32_t nb = (w + 3) / 4 * ((h + 3) / 4);
        uint8_t *blocks = malloc((size_t)nb * 16);
        CHECK(blocks);
        encode(px, w, h, blocks);
        f = fopen(argv[5], "wb");
        CHECK(f && fwrite(blocks, 16, nb, f) == nb);
        fclose(f);
        printf("encoded %ux%u: %u blocks\n", w, h, nb);
        return 0;
    }
    static const uint32_t sizes[][2] = {{64, 64}, {37, 29}, {128, 96}, {4, 4}, {1, 1}, {6, 3}};
    int failed = 0;
    for (int kind = 0; kind < K_KINDS; kind++) {
        double worst = 1e9;
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            uint32_t w = sizes[s][0], h = sizes[s][1], bx = (w + 3) / 4, by = (h + 3) / 4;
            uint8_t *px = malloc((size_t)w * h * 4), *blocks = malloc((size_t)bx * by * 16);
            CHECK(px && blocks);
            make_image(kind, w, h, px);
            encode(px, w, h, blocks);
            double se = 0;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t t[16][4];
                    if (!decode_mode6(blocks + 16 * ((y / 4) * bx + x / 4), t)) {
                        printf("%s %ux%u: a block is not mode 6\n", k_kind[kind], w, h);
                        return 1;
                    }
                    const uint8_t *q = t[(y & 3) * 4 + (x & 3)], *o = px + 4 * (y * w + x);
                    for (int c = 0; c < 4; c++) se += (double)(q[c] - o[c]) * (q[c] - o[c]);
                }
            double mse = se / ((double)w * h * 4);
            double psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99.0;
            if (w * h >= 64 && psnr < worst) worst = psnr;   // the 1x1, 4x4 and 6x3 images hold one or two blocks of a whole range of colour: only their mode is checked
            if (w * h >= 64 && psnr < k_floor[kind]) {
                printf("%s %ux%u: PSNR %.1f dB is under its floor %.1f\n", k_kind[kind], w, h, psnr, k_floor[kind]);
                failed = 1;
            }
            free(px), free(blocks);
        }
        printf("%-12s worst PSNR %.1f dB (floor %.1f)\n", k_kind[kind], worst, k_floor[kind]);
    }
    if (failed) return 1;
    printf("ok bc7_encode\n");
    return 0;
}
