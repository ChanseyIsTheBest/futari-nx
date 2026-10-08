#!/usr/bin/env python3
"""Wraps the raw ring microcode the native renderer discovered (FUTARI_SHADER_DISCOVERY: <stage>_<hash>.ucode)
into register-linked containers for the shader library, but ONLY the shaders no real container covers.

Why the filter matters: a raw discovery is often a vertex shader Direct3D rewrote for a vertex declaration, whose
real container also exists (the dump from CreateVertexShader, a variant repaired by repair_vertex_variant_declarations,
or the rewritten container FUTARI_SHADER_MISSING keeps). Packed together, the library's code index prefers the
register-linked wrapper when two entries share their microcode, and a register-linked vertex shader paired with the
game's semantic-linked pixel shader passes its interpolators by the wrong location: SA2's health warning, SEGA and Sonic
Team screens came out black (the texture coordinates arrived as zero). Only Direct3D's own shaders, which have no
container anywhere, are wrapped.

  python tools/wrap_discovered.py --raw out/pc/shaders_discovery out/pc/shaders_discovery2 \
      --containers out/pc/shaders out/pc/shaders_missing out/pc/shaders_repaired --out out/pc/shaders_wrapped
"""
import argparse
import glob
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def physical_part(container):
    if len(container) < 12:
        return b''
    virtual = struct.unpack_from('>I', container, 4)[0]
    physical = struct.unpack_from('>I', container, 8)[0]
    return container[virtual:virtual + physical]


def pixel_exports(w):
    """banjo-nx: bit mask of a pixel shader's exports (bits 0..3 colour targets, bit 4 depth = export register 61), found
    by walking the exec clauses. wrap_raw_shader.py alone always declares colour 0 only."""
    mask = 0
    end = len(w) // 3
    pair = 0
    while pair < end and pair * 3 + 2 < len(w):
        cf = [w[pair * 3] | ((w[pair * 3 + 1] & 0xFFFF) << 32),
              (w[pair * 3 + 1] >> 16) | (w[pair * 3 + 2] << 16)]
        for c in cf:
            opcode = (c >> 44) & 0xF
            if not ((1 <= opcode <= 6) or opcode in (13, 14)):
                continue
            address, count, sequence = c & 0xFFF, (c >> 12) & 0x7, (c >> 16) & 0xFFF
            if address:
                end = min(end, address)
            for i in range(count):
                index = address + i
                if (sequence >> (i * 2)) & 1 or index * 3 + 2 >= len(w):
                    continue  # a fetch, or outside the program
                w0 = w[index * 3]
                if not (w0 >> 15) & 1:
                    continue  # not an export
                for dest, write in (((w0 & 0x3F), (w0 >> 16) & 0xF), (((w0 >> 8) & 0x3F), (w0 >> 20) & 0xF)):
                    if not write:
                        continue
                    if dest <= 3:
                        mask |= 1 << dest
                    elif dest == 61:
                        mask |= 0x10
        pair += 1
    return mask or 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--raw', nargs='+', required=True, help='folders with <stage>_<hash>.ucode files')
    ap.add_argument('--containers', nargs='+', required=True, help='folders with real containers (*.bin)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    covered = []
    for d in a.containers:
        for p in glob.glob(os.path.join(d, '*.bin')):
            covered.append((os.path.basename(p), physical_part(open(p, 'rb').read())))
    templates = {s: sorted(glob.glob(os.path.join(a.containers[0], f'{s}_*.bin'))) for s in ('vs', 'ps')}
    os.makedirs(a.out, exist_ok=True)
    for old in glob.glob(os.path.join(a.out, '*.bin')):
        os.remove(old)  # the folder is regenerated from scratch, so a skipped shader cannot linger
    wrapped = skipped = 0
    seen = set()
    for d in a.raw:
        for p in sorted(glob.glob(os.path.join(d, '*.ucode'))):
            name = os.path.basename(p)[:-len('.ucode')]
            if name in seen:
                continue
            seen.add(name)
            raw = open(p, 'rb').read()
            owner = next((n for n, phys in covered if raw and raw in phys), None)
            if owner:
                print(f'skip {name}: its microcode is in the real container {owner}')
                skipped += 1
                continue
            stage = name.split('_', 1)[0]
            if not templates.get(stage):
                sys.exit(f'no {stage} template container in {a.containers[0]}')
            target = os.path.join(a.out, name + '.bin')
            command = [sys.executable, os.path.join(ROOT, 'shaders', 'tools', 'wrap_raw_shader.py'), p,
                       templates[stage][0], target, '--stage', stage]
            exports = 0
            if stage == 'ps':
                exports = pixel_exports(list(struct.unpack('>%dI' % (len(raw) // 4), raw)))
                if exports & 0x10:
                    command.append('--depth')
            subprocess.run(command, check=True)
            if stage == 'ps':
                container = bytearray(open(target, 'rb').read())
                shader = struct.unpack_from('>I', container, 24)[0]
                struct.pack_into('>I', container, shader + 28, exports)
                open(target, 'wb').write(bytes(container))
            print(f'wrap {name}' + (f' (exports {exports:02X})' if stage == 'ps' else ''))
            wrapped += 1
    print(f'{wrapped} wrapped (Direct3D\'s own shaders), {skipped} skipped (a real container has them)')


if __name__ == '__main__':
    main()
