#!/usr/bin/env python3
"""Upscale the HUD textures the game showed (docs/hud.md).

The window writes every 2D texture it meets to hud_dump/<key>.png when PS2X_HUD_DUMP=1 (run_rayman_hd.bat sets it).
This runs Real-ESRGAN (realesrgan-ncnn-vulkan, model realesrgan-x4plus-anime: made for drawn images like the game's
font and icons) on the ones not upscaled yet and writes hud_textures/<key>.png, which the window then uses instead
of the original (same key, 4x the size; texture coordinates are normalized, so any size works).

Usage (from the repo folder): python tools/hud_upscale.py [--model realesrgan-x4plus-anime] [--force]
Needs tools/realesrgan/realesrgan-ncnn-vulkan.exe (the Windows build of https://github.com/xinntao/Real-ESRGAN,
release v0.2.5.0, extracted there; not in git). Textures are game content: hud_dump/ and hud_textures/ are not
committed either.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, 'tools', 'realesrgan', 'realesrgan-ncnn-vulkan.exe')
DUMP = os.path.join(ROOT, 'hud_dump')
OUT = os.path.join(ROOT, 'hud_textures')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', default='realesrgan-x4plus-anime')
    parser.add_argument('--force', action='store_true', help='upscale again the ones already done')
    args = parser.parse_args()

    if not os.path.isfile(TOOL):
        sys.exit('Real-ESRGAN not found: %s (see the top of this file)' % TOOL)
    if not os.path.isdir(DUMP):
        sys.exit('No hud_dump folder yet: play with PS2X_HUD_DUMP=1 (run_rayman_hd.bat) first.')
    os.makedirs(OUT, exist_ok=True)
    todo = [f for f in sorted(os.listdir(DUMP))
            if f.lower().endswith('.png') and (args.force or not os.path.exists(os.path.join(OUT, f)))]
    if not todo:
        print('Nothing new to upscale (%d textures already done).' % len(os.listdir(OUT)))
        return
    print('Upscaling %d textures with %s ...' % (len(todo), args.model))
    # The tool works on a whole folder: copy the new ones into a temporary one.
    with tempfile.TemporaryDirectory() as work:
        src = os.path.join(work, 'in')
        dst = os.path.join(work, 'out')
        os.makedirs(src)
        os.makedirs(dst)
        for f in todo:
            shutil.copyfile(os.path.join(DUMP, f), os.path.join(src, f))
        result = subprocess.run([TOOL, '-i', src, '-o', dst, '-n', args.model, '-s', '4', '-f', 'png'],
                                cwd=os.path.dirname(TOOL))
        if result.returncode != 0:
            sys.exit('Real-ESRGAN failed (exit code %d)' % result.returncode)
        done = 0
        for f in todo:
            produced = os.path.join(dst, f)
            if os.path.exists(produced):
                shutil.move(produced, os.path.join(OUT, f))
                done += 1
    print('Done: %d new textures in %s. They are used the next time the game starts.' % (done, OUT))


if __name__ == '__main__':
    main()
