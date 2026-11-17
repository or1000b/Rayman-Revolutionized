// SPDX-FileCopyrightText: 2024 Arntzen Software AS
// SPDX-FileContributor: Hans-Kristian Arntzen
// SPDX-FileContributor: Runar Heyer
// SPDX-License-Identifier: LGPL-3.0+
//
// PS2Recomp: paraLLEl-GS's sample_circuit.frag for a 4x scanout (PS2X_GS_SSAA=16, 2026-11-17). With 16 samples on the
// ordered grid each PS2 pixel holds a 4 x 4 grid of samples; the original shader scans out at 2x and averages the 4
// samples of each quarter. Here every output pixel takes the one sample at its place, so a PAL picture comes out at
// 2560 x 2048. Compiled to SPIR-V by tools/pgs_shaders/compile_scanout4x.py (sample_circuit_4x_spirv.h); the interface
// (bindings, push constants, specialization constants) is the original's, so its reflection layout is reused.

#version 450

#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_samplerless_texture_functions : require
#include "data_structures.h"
#include "swizzle_utils.h"
#include "utils.h"

layout(location = 0) out vec4 FragColor;

#if PROMOTED
layout(set = 0, binding = 0) uniform texture2DArray uPromoted;
#else
layout(set = 0, binding = 0) readonly buffer VRAM32
{
    uint data[];
} vram32;

layout(set = 0, binding = 0) readonly buffer VRAM16
{
    uint16_t data[];
} vram16;
#endif

layout(push_constant) uniform Registers
{
    uint fbp;
    uint fbw;
    uint dbx;
    uint dby;
    uint phase;
    uint phase_stride;
} registers;

layout(constant_id = 0) const int PSM = 0;
layout(constant_id = 1) const uint VRAM_MASK = 4 * 1024 * 1024 - 1;
layout(constant_id = 2) const uint SUPER_SAMPLES = 16;

const bool is_tex_16bit = PSM == PSMCT16 || PSM == PSMCT16S || PSM == PSMZ16 || PSM == PSMZ16S;

#if PROMOTED
vec4 sample_vram(uvec2 coord, uint slice)
{
    return texelFetch(uPromoted, ivec3(coord, slice), 0);
}
#else
vec4 sample_vram(uint addr, uint slice)
{
    uint payload;

    if (is_tex_16bit)
    {
        payload = rgba16_to_rgba32(uint(vram16.data[addr + slice * (VRAM_MASK + 1) / 2]), 0, 0x0, 0xff);
    }
    else
    {
        payload = vram32.data[addr + slice * (VRAM_MASK + 1) / 4];
        if (PSM != PSMCT32)
        {
            payload &= ~0xff000000u;
            payload |= 0x80000000u;
        }
    }

    return unpackUnorm4x8(payload);
}
#endif

#if PROMOTED
bool super_sample_is_valid(uvec2 coord) { return true; }
#else
bool super_sample_is_valid(uint addr)
{
    bool is_valid;

    if (is_tex_16bit)
    {
        is_valid = uint(vram16.data[addr + (VRAM_MASK + 1) / 2]) == 0xffff;
    }
    else
    {
        uint payload = vram32.data[addr + (VRAM_MASK + 1) / 4];
        if (PSM != PSMCT32)
            is_valid = (payload & 0xffffffu) == 0xffffffu;
        else
            is_valid = payload == ~0u;
    }

    return is_valid;
}
#endif

void main()
{
    uvec2 super_sampled_coord = uvec2(gl_FragCoord.xy);
    uvec2 single_sampled_coord = super_sampled_coord >> 2;

    uvec2 coord = single_sampled_coord * uvec2(1u, registers.phase_stride) +
        uvec2(registers.dbx, registers.dby + registers.phase);

#if PROMOTED
    #define addr coord
    const int BASE_SSAA_LAYER = 1;
#else
    uint addr = swizzle_PS2(coord.x, coord.y, registers.fbp * PGS_BLOCKS_PER_PAGE, registers.fbw, PSM, VRAM_MASK);
    const int BASE_SSAA_LAYER = 2;
#endif

    if (super_sample_is_valid(addr))
    {
        // The ubershader's sample order (ordered 4 x 4 grid): sample bits 0 / 2 = row bits 0 / 1, bits 1 / 3 = column
        // bits 0 / 1 (sample_circuit.frag's 2x scanout averages samples 4q .. 4q + 3 for quarter q = row bit 1 +
        // 2 * column bit 1, the same order).
        uvec2 s = super_sampled_coord & 3u;
        uint sample_index = (s.y & 1u) + (s.x & 1u) * 2u + (s.y >> 1u) * 4u + (s.x >> 1u) * 8u;
        FragColor = sample_vram(addr, BASE_SSAA_LAYER + sample_index);
    }
    else
        FragColor = sample_vram(addr, 0);
}
