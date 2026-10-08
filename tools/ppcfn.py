#!/usr/bin/env python3
"""Prints a recompiled function's PowerPC listing (the codegen's '// insn' comments, with its labels) from
app/generated/<module>/: the reference the native Direct3D layer is transliterated against (sa2-nx's ppcfn.py).

  python tools/ppcfn.py ADDRESS [--module default|reco_abnormal|...] [--calls] [--count]

--calls lists only the calls (bl targets); --count prints the instruction count only.
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'app', 'generated')


def function_text(module, address):
    name = 'sub_%08X' % address
    start = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?%s\)' % name)
    for path in sorted(glob.glob(os.path.join(GEN, module, 'futari_recomp.*.cpp'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        for i, line in enumerate(lines):
            if start.match(line):
                out = []
                for line2 in lines[i + 1:]:
                    if line2.startswith('}'):
                        break
                    out.append(line2)
                return out
    return None


def listing(lines):
    for line in lines:
        s = line.strip()
        if s.startswith('// '):
            yield '    ' + s[3:]
        elif re.match(r'^loc_[0-9A-F]{8}:', s):
            yield s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('address')
    ap.add_argument('--module', default='default')
    ap.add_argument('--calls', action='store_true')
    ap.add_argument('--count', action='store_true')
    a = ap.parse_args()
    address = int(a.address.replace('sub_', ''), 16)
    lines = function_text(a.module, address)
    if lines is None:
        raise SystemExit('sub_%08X not found in %s' % (address, a.module))
    asm = list(listing(lines))
    if a.count:
        print(sum(1 for x in asm if not x.startswith('loc_')))
        return
    for x in asm:
        if a.calls and not re.match(r'\s+bl\s', x):
            continue
        print(x)


if __name__ == '__main__':
    main()
