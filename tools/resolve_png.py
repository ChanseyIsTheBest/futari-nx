#!/usr/bin/env python3
"""Turns the resolve dumps of the native renderer (futari_dump_resolves_s: captures/resolve_<n>_<base>_<w>x<h>_vk<f>.raw)
into PNG files beside them, plus an alpha-only PNG (<name>.alpha.png) when the format has alpha.

  python tools/resolve_png.py [folder]        (default: out/pc/captures)

Vulkan formats handled: 37 R8G8B8A8_UNORM, 64 A2B10G10R10_UNORM_PACK32, 8 A1R5G5B5_UNORM_PACK16,
4 R5G6B5_UNORM_PACK16, 97 R16G16B16A16_SFLOAT. Needs ffmpeg (FFMPEG or C:/ffmpeg/ffmpeg.exe).
"""
import array
import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FFMPEG = os.environ.get('FFMPEG', 'C:/ffmpeg/ffmpeg.exe')


def half(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]


def to_rgba8(data, w, h, vk):
    n = w * h
    out = bytearray(n * 4)
    if vk == 37:
        return bytes(data[:n * 4])
    if vk == 64:
        words = array.array('I', data[:n * 4])
        for i, v in enumerate(words):
            out[i * 4:i * 4 + 4] = bytes(((v & 0x3FF) >> 2, ((v >> 10) & 0x3FF) >> 2, ((v >> 20) & 0x3FF) >> 2,
                                          (v >> 30) * 85))
        return bytes(out)
    if vk in (8, 4):
        words = array.array('H', data[:n * 2])
        for i, v in enumerate(words):
            if vk == 8:  # B 0-4, G 5-9, R 10-14, A 15
                r, g, b, a = (v >> 10) & 31, (v >> 5) & 31, v & 31, 255 if v >> 15 else 0
                out[i * 4:i * 4 + 4] = bytes((r * 255 // 31, g * 255 // 31, b * 255 // 31, a))
            else:  # B 0-4, G 5-10, R 11-15
                r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
                out[i * 4:i * 4 + 4] = bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31, 255))
        return bytes(out)
    if vk == 97:
        hs = array.array('H', data[:n * 8])
        for i in range(n):
            px = [min(max(half(hs[i * 4 + c]), 0.0), 1.0) for c in range(4)]
            out[i * 4:i * 4 + 4] = bytes(int(c * 255 + 0.5) for c in px)
        return bytes(out)
    return None


def write_png(rgba, w, h, path, alpha=False):
    if alpha:
        rgba = bytes(b for i in range(0, len(rgba), 4) for b in (rgba[i + 3],) * 3 + (255,))
    subprocess.run([FFMPEG, '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgba', '-s', f'{w}x{h}',
                    '-i', '-', path], input=rgba, check=True)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'out', 'pc', 'captures')
    for raw in sorted(glob.glob(os.path.join(folder, 'resolve_*.raw'))):
        m = re.search(r'_(\d+)x(\d+)_vk(\d+)\.raw$', raw)
        if not m:
            continue
        w, h, vk = map(int, m.groups())
        rgba = to_rgba8(open(raw, 'rb').read(), w, h, vk)
        if rgba is None:
            print(f'{os.path.basename(raw)}: format {vk} not handled')
            continue
        write_png(rgba, w, h, raw[:-4] + '.png')
        if vk in (37, 64, 8, 97):
            write_png(rgba, w, h, raw[:-4] + '.alpha.png', alpha=True)
        print(f'{os.path.basename(raw)} -> png')


if __name__ == '__main__':
    main()
