#!/usr/bin/env python3
"""Finds the copies of a guest function in every module (default.xex and the eight game cores): the cores carry their
own CRT and game library, so one native rewrite usually applies in up to nine places, each at its own address.

Two functions are the same when their PPC instructions (the `// insn` comments of the generated code) are equal after
branch targets are made relative to the function start.

  python tools/find_same_functions.py default:sub_8207DAC0 [palm_normal:sub_88012340 ...] [--gen out/generated_baseline]
"""
import argparse
import glob
import hashlib
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_([0-9A-F]{8}))\)')
ASM = re.compile(r'^\s*// (.*\S)\s*$')
TARGET = re.compile(r'0x(8[0-9a-f]{7})\b')


def functions(folder):
    """{name: (start, [normalized insns])} of one module folder."""
    out = {}
    for path in sorted(glob.glob(os.path.join(folder, '*_recomp.*.cpp'))):
        name, start, body = None, 0, []
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                if name:
                    out[name] = (start, body)
                name, start, body = m.group(1), int(m.group(2), 16), []
                continue
            if not name:
                continue
            a = ASM.match(line)
            if a:
                body.append(a.group(1))
        if name:
            out[name] = (start, body)
    return out


def key(start, body):
    text = '\n'.join(TARGET.sub(lambda m: f'@{int(m.group(1), 16) - start:+x}', insn) for insn in body)
    return hashlib.sha1(text.encode()).hexdigest(), len(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('functions', nargs='+', help='module:sub_XXXXXXXX')
    ap.add_argument('--gen', default=os.path.join(ROOT, 'out', 'generated_baseline'))
    a = ap.parse_args()
    modules = {os.path.basename(p.rstrip('/\\')): functions(p) for p in sorted(glob.glob(os.path.join(a.gen, '*', '')))}
    index = {}
    for module, fns in modules.items():
        for name, (start, body) in fns.items():
            index.setdefault(key(start, body), []).append(f'{module}:{name}')
    for spec in a.functions:
        module, name = spec.split(':')
        if name not in modules.get(module, {}):
            print(f'{spec}: not found')
            continue
        k = key(*modules[module][name])
        print(f'{spec} ({k[1]} instructions): ' + ' '.join(index[k]))


if __name__ == '__main__':
    main()
