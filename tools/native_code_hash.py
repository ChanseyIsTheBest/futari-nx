#!/usr/bin/env python3
"""Code-check hash of a native replacement (futari_native.h Site::hash): FNV-1a 64 over the guest instruction words,
read from the module images in out/ (<module>_image.bin, made with FUTARI_DUMP_IMAGE / the pointer-scan tooling).

  python tools/native_code_hash.py default:8207DAC0:290 palm_normal:88072270:290 ...

Prints the hash and the first instructions' words (to check the address is a function start).
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASES = {'default': 0x82000000}


def main():
    for spec in sys.argv[1:]:
        module, address, words = spec.split(':')
        address, words = int(address, 16), int(words)
        base = BASES.get(module, 0x88000000)
        data = open(os.path.join(ROOT, 'out', f'{module}_image.bin'), 'rb').read()
        code = data[address - base:address - base + words * 4]
        h = 0xcbf29ce484222325
        for b in code:
            h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        first = ' '.join(f'{w:08X}' for w in struct.unpack('>4I', code[:16]))
        print(f'{module}:{address:08X} {words} words: 0x{h:016X}ull  ({first} ...)')


if __name__ == '__main__':
    main()
