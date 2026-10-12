// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    float power; // wbp: applied to the encoded result (frame generation's HUD-less copy)
    bool use_lut; // wbp: the encoded result through `lut` (the game's curve after its HUD)
} pp;

layout (std430, binding = 1) readonly buffer Lut {
    uint lut[256];
};

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
        if (pp.power != 1.0) color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(pp.power));
        if (pp.use_lut) {
            const uvec3 i = uvec3(round(clamp(color.rgb, 0.0, 1.0) * 255.0));
            color.rgb = vec3(lut[i.r] & 255u, (lut[i.g] >> 8) & 255u, (lut[i.b] >> 16) & 255u) /
                        255.0;
        }
    }
}
