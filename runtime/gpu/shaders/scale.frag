// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The oot2-native contributors
//
// The picture scaler's fragment stage (runtime/gpu/vk_scale.c): the game's picture enlarged to the window with a
// Catmull-Rom bicubic (five bilinear taps, after Jimenez), then sharpened by a contrast-adaptive filter (after AMD's CAS
// idea: the less contrast a pixel already has against its four neighbours, the more it is sharpened, and the sharpening
// stops short of clipping) so that the enlargement keeps its edges. Spatial only: it adds no detail the game did not draw.
#version 450

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push {
    vec4 rect;       // the picture's rectangle in the window, in pixels: x, y, width, height
    vec2 src_size;   // the picture's size in texels
    float sharp;     // 0 to 1
    float pad;
} pc;

layout(location = 0) out vec4 out_color;

vec3 catmull_rom(vec2 uv) {
    vec2 pos = uv * pc.src_size;
    vec2 t1 = floor(pos - 0.5) + 0.5;
    vec2 f = pos - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 inv = 1.0 / pc.src_size;
    vec2 t0 = (t1 - 1.0) * inv;
    vec2 t3 = (t1 + 2.0) * inv;
    vec2 t12 = (t1 + w2 / w12) * inv;
    float k0 = w12.x * w0.y, k1 = w0.x * w12.y, k2 = w12.x * w12.y, k3 = w3.x * w12.y, k4 = w12.x * w3.y;
    vec3 c = texture(src, vec2(t12.x, t0.y)).rgb * k0 + texture(src, vec2(t0.x, t12.y)).rgb * k1 +
             texture(src, vec2(t12.x, t12.y)).rgb * k2 + texture(src, vec2(t3.x, t12.y)).rgb * k3 +
             texture(src, vec2(t12.x, t3.y)).rgb * k4;
    return c / (k0 + k1 + k2 + k3 + k4);
}

void main() {
    vec2 uv = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw;
    vec2 d = 1.0 / pc.rect.zw;   // one window pixel, in picture coordinates
    vec3 c = clamp(catmull_rom(uv), 0.0, 1.0);
    if (pc.sharp <= 0.0) {
        out_color = vec4(c, 1.0);
        return;
    }
    vec3 u = clamp(catmull_rom(uv + vec2(0.0, -d.y)), 0.0, 1.0);
    vec3 b = clamp(catmull_rom(uv + vec2(0.0, d.y)), 0.0, 1.0);
    vec3 l = clamp(catmull_rom(uv + vec2(-d.x, 0.0)), 0.0, 1.0);
    vec3 r = clamp(catmull_rom(uv + vec2(d.x, 0.0)), 0.0, 1.0);
    vec3 mn = min(c, min(min(u, b), min(l, r)));
    vec3 mx = max(c, max(max(u, b), max(l, r)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 w = amp * (-1.0 / mix(8.0, 5.0, pc.sharp));
    out_color = vec4(clamp((c + (u + b + l + r) * w) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}
