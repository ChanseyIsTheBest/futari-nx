#!/usr/bin/env python3
"""The C runtime's memcpy and memset of a generated module folder, for tools/inline_memcalls.py (futari-nx, 2026-10-19).
Every Futari module links its own copy of the XDK's LIBCMT, so each has its own two addresses. Found by their call
sites and their code, not by name (the generator names neither):
  - memcpy: the CRT routine starts with `std r3,-8(r1)` (it returns r3 from that slot: banjo-nx's description of the
    same XDK routine), and is called with r3 = destination, r4 = source, r5 = size;
  - memset: the routine that starts `addi r0,r5,1; mtctr r0; ori r6,r3,0` (banjo-nx's memset, the same code).
Each must be the only function of its shape with direct calls; the one with the most direct calls whose r5 is a
constant set just before them is taken, and the candidates are printed so a wrong pick is visible.

  python tools/find_memcalls.py <generated module folder> [--json]
"""
import collections
import glob
import json
import os
import re
import sys

HEAD = re.compile(r'^DEFINE_REX_FUNC\((?:__imp__)?(sub_[0-9A-F]{8})\) \{$')
COMMENT = re.compile(r'^\t// (\S+)\s*(.*)$')
CALL = re.compile(r'^\t+(?:__imp__)?(sub_[0-9A-F]{8})\(ctx, base\);$')


def first_instructions(gen):
    """name -> its first 8 guest instructions (mnemonic, operands)."""
    out = {}
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        name = None
        for line in open(path, encoding='utf-8'):
            line = line.rstrip('\n')
            m = HEAD.match(line)
            if m:
                name = m.group(1)
                out[name] = []
                continue
            if name and len(out[name]) < 8:
                c = COMMENT.match(line)
                if c:
                    out[name].append((c.group(1), c.group(2).strip()))
            if line == '}':
                name = None
    return out


def call_sites(gen):
    """callee -> Counter of the guest instructions setting r4 in the straight line before the call (with r5 set by
    an `li r5,N` there too)."""
    out = collections.defaultdict(collections.Counter)
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        recent = []
        for line in open(path, encoding='utf-8'):
            line = line.rstrip('\n')
            if line.startswith(('loc_', 'DEFINE_REX_FUNC', '}')):
                recent = []
                continue
            c = COMMENT.match(line)
            if c:
                recent = (recent + [(c.group(1), c.group(2).strip())])[-6:]
                continue
            m = CALL.match(line)
            if m and recent and recent[-1][0] == 'bl':
                r5 = any(mn == 'li' and ops.startswith('r5,') for mn, ops in recent[:-1])
                r4 = next((mn + ' ' + ops for mn, ops in reversed(recent[:-1]) if ops.startswith('r4,')), None)
                if r5:
                    out[m.group(1)][r4 or '-'] += 1
    return out


def find(gen):
    first = first_instructions(gen)
    sites = call_sites(gen)
    memcpy = [(sum(c.values()), n) for n, c in sites.items() if first.get(n) and first[n][0] == ('std', 'r3,-8(r1)')]
    # memset by its code: the XDK routine (banjo-nx's sub_82BB4A40, instruction for instruction) starts
    # `addi r0,r5,1; mtctr r0; ori r6,r3,0` (r3 is never written: the routine returns the destination untouched).
    shape = [('addi', 'r0,r5,1'), ('mtctr', 'r0'), ('ori', 'r6,r3,0')]
    memset = [(sum(sites[n].values()) if n in sites else 0, n) for n, ins in first.items() if ins[:3] == shape]
    memcpy.sort(reverse=True)
    memset.sort(reverse=True)
    return memcpy, memset, first


def main():
    gen = sys.argv[1]
    memcpy, memset, first = find(gen)
    if '--json' in sys.argv:
        print(json.dumps({'memcpy': memcpy[0][1] if memcpy else None, 'memset': memset[0][1] if memset else None}))
        return
    for label, found in (('memcpy', memcpy), ('memset', memset)):
        print('%s candidates (calls with a constant r5): %s' % (label, ', '.join('%s %d' % (n, k) for k, n in found[:5])))
        if found:
            print('  %s starts: %s' % (found[0][1], '; '.join('%s %s' % i for i in first[found[0][1]])))


if __name__ == '__main__':
    main()
