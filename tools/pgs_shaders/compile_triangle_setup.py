"""Compile paraLLEl-GS's triangle_setup.comp with one change for Rayman and write triangle_setup_spirv.h, which
PatchParallelGs.cmake hands to paraLLEl-GS in place of its own precompiled triangle setup.

The change (2026-11-18, user report: glows, sparkles and Rayman's energy balls look pixelated at quality 16): with
super-sampling, paraLLEl-GS takes a textured primitive whose three vertices have the same Z and no change of Q for flat
UI and snaps its attribute interpolation to the PS2 pixel (one colour per PS2 pixel, "There is no point in upsampling UI
elements anyway"). A camera-facing billboard (a glow, a sparkle, the energy ball) is just as flat, so every one came out
at PS2 resolution: blocks of 4 x 4 screen pixels at the 4x scanout. Rayman draws all its 2D (menus, text, the HUD) with
every vertex at Z = 0xFFFF (gs_hud.cpp relies on the same), so here only that Z counts as flat UI.

Uses the shaderc library through ctypes like compile_scanout4x.py (paraLLEl-GS ships its shaders precompiled and no
GLSL compiler comes with this repository). Only needed when paraLLEl-GS or the change moves: the generated header is
committed.

Usage: python tools/pgs_shaders/compile_triangle_setup.py [--shaderc PATH] [--deps out/game/_deps]
"""
import argparse
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compile_scanout4x import ROOT, inline_includes  # noqa: E402

OUTPUT = os.path.join(ROOT, 'ps2xRuntime', 'src', 'lib', 'gs', 'pgs', 'shaders', 'triangle_setup_spirv.h')

ANCHOR = '''        if (!should_interpolate_per_sample)
            should_interpolate_per_sample = za != zb || zb != zc;
'''
REPLACEMENT = ANCHOR + '''        // PS2RECOMP-PATCH flat-ui-z: Rayman draws all its 2D at Z = 0xFFFF. A flat primitive at any other depth is a 3D
        // billboard (glows, sparkles, the energy ball): per-sample interpolation, or it comes out at PS2 resolution.
        if (!should_interpolate_per_sample)
            should_interpolate_per_sample = za != 0xffffu;
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--shaderc', default=r'D:\PCSX2\shaderc_shared.dll')
    ap.add_argument('--deps', default=os.path.join(ROOT, 'out', 'game', '_deps'))
    args = ap.parse_args()
    shader_dir = os.path.join(args.deps, 'ps2x_parallel_gs-src', 'gs', 'shaders')
    search = [shader_dir, os.path.join(args.deps, 'ps2x_granite-src', 'assets', 'shaders', 'inc')]
    path = os.path.join(shader_dir, 'triangle_setup.comp')

    source = inline_includes(path, search, set())
    if source.count(ANCHOR) != 1:
        sys.exit('anchor not found once in %s: paraLLEl-GS changed, update the patch' % path)
    source = source.replace(ANCHOR, REPLACEMENT).encode('utf-8')

    sc = ctypes.CDLL(args.shaderc)
    sc.shaderc_compiler_initialize.restype = ctypes.c_void_p
    sc.shaderc_compile_options_initialize.restype = ctypes.c_void_p
    sc.shaderc_compile_options_set_target_env.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32]
    sc.shaderc_compile_options_set_target_spirv.argtypes = [ctypes.c_void_p, ctypes.c_int]
    sc.shaderc_compile_options_set_optimization_level.argtypes = [ctypes.c_void_p, ctypes.c_int]
    sc.shaderc_compile_options_release.argtypes = [ctypes.c_void_p]
    sc.shaderc_compile_into_spv.restype = ctypes.c_void_p
    sc.shaderc_compile_into_spv.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int,
                                            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
    sc.shaderc_result_get_compilation_status.argtypes = [ctypes.c_void_p]
    sc.shaderc_result_get_length.restype = ctypes.c_size_t
    sc.shaderc_result_get_length.argtypes = [ctypes.c_void_p]
    sc.shaderc_result_get_bytes.restype = ctypes.c_void_p
    sc.shaderc_result_get_bytes.argtypes = [ctypes.c_void_p]
    sc.shaderc_result_get_error_message.restype = ctypes.c_char_p
    sc.shaderc_result_get_error_message.argtypes = [ctypes.c_void_p]
    sc.shaderc_result_release.argtypes = [ctypes.c_void_p]

    compiler = sc.shaderc_compiler_initialize()
    opts = sc.shaderc_compile_options_initialize()
    sc.shaderc_compile_options_set_target_env(opts, 0, (1 << 22) | (1 << 12))  # Vulkan 1.1
    sc.shaderc_compile_options_set_target_spirv(opts, 0x10300)                  # SPIR-V 1.3, like paraLLEl-GS's
    sc.shaderc_compile_options_set_optimization_level(opts, 2)                  # performance
    result = sc.shaderc_compile_into_spv(compiler, source, len(source), 2, b'triangle_setup.comp', b'main', opts)  # 2 = compute
    if sc.shaderc_result_get_compilation_status(result) != 0:
        sys.exit('compile failed:\n%s' % sc.shaderc_result_get_error_message(result).decode())
    n = sc.shaderc_result_get_length(result)
    data = ctypes.string_at(sc.shaderc_result_get_bytes(result), n)
    sc.shaderc_result_release(result)
    sc.shaderc_compile_options_release(opts)
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, n, 4)]
    print(f'{n} bytes of SPIR-V')

    lines = ['// Generated by tools/pgs_shaders/compile_triangle_setup.py from paraLLEl-GS triangle_setup.comp with the',
             '// flat-ui-z change (only Z = 0xFFFF counts as flat UI): do not edit.',
             '#pragma once', '#include <stdint.h>', '',
             f'static const uint32_t ps2x_triangle_setup_spirv[{len(words)}] = {{']
    for j in range(0, len(words), 8):
        lines.append('\t' + ', '.join(f'0x{w:08x}u' for w in words[j:j + 8]) + ',')
    lines += ['};', '']
    open(OUTPUT, 'w', newline='\n').write('\n'.join(lines))
    print('wrote', OUTPUT)


if __name__ == '__main__':
    main()
