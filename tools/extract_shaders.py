#!/usr/bin/env python3
"""Extracts the Direct3D shader containers compiled into the game's executables, for the native renderer's shader
package (tools/build_shader_library.sh).

Mushihimesama Futari keeps its shaders inside the images: default.xex holds 22 vertex + 22 pixel shader containers in
its data, and every game core in media/Module/ 2 + 2 of its own. Each is the blob D3DDevice_CreateVertexShader /
CreatePixelShader receives, an XDK 2.0.9328 container:

  +0  magic 0x102A1101 (vertex) / 0x102A1100 (pixel)
  +4  header size: the microcode starts there
  +8  microcode size

So no run-time dump (the cvar other ports use) is needed: the files are written in the same format,
vs_<xxh3 of the blob>.bin / ps_<...>.bin, to one folder for all binaries (identical containers collapse into one).

  python tools/extract_shaders.py [--out out/shader_dump] [images ...]     (default: out/*_image.bin)
"""
import argparse
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import xxhash  # optional; the names only need to be stable and unique

    def blob_hash(b):
        return xxhash.xxh3_64_intdigest(b)
except ImportError:
    import hashlib

    def blob_hash(b):
        return int.from_bytes(hashlib.blake2b(b, digest_size=8).digest(), 'big')

MAGIC = {0x102A1101: 'vs', 0x102A1100: 'ps'}


def containers(image):
    for off in range(0, len(image) - 12, 4):
        magic = struct.unpack_from('>I', image, off)[0]
        if magic not in MAGIC:
            continue
        header, ucode = struct.unpack_from('>II', image, off + 4)
        if not 12 <= header <= (1 << 16) or not 0 < ucode <= (1 << 20) or ucode % 4:
            print(f'  skipped implausible container at +0x{off:X} (header {header}, microcode {ucode})',
                  file=sys.stderr)
            continue
        if off + header + ucode > len(image):
            continue
        yield off, MAGIC[magic], image[off:off + header + ucode]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('images', nargs='*')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'shader_dump'))
    a = ap.parse_args()
    images = a.images or sorted(glob.glob(os.path.join(ROOT, 'out', '*_image.bin')))
    os.makedirs(a.out, exist_ok=True)
    seen = {}
    for path in images:
        image = open(path, 'rb').read()
        n = new = 0
        for off, kind, blob in containers(image):
            n += 1
            name = f'{kind}_{blob_hash(blob):016x}.bin'
            if name not in seen:
                new += 1
                seen[name] = f'{os.path.basename(path)}+0x{off:X}'
                open(os.path.join(a.out, name), 'wb').write(blob)
        print(f'{os.path.basename(path)}: {n} containers, {new} new')
    with open(os.path.join(a.out, 'sources.txt'), 'w') as f:
        for name, src in sorted(seen.items()):
            f.write(f'{name} {src}\n')
    print(f'{len(seen)} distinct containers in {a.out}')


if __name__ == '__main__':
    main()
