#!/usr/bin/env python3
"""The per-module addresses of default.xex functions in the eight game cores (each core links its own Direct3D at its
own addresses): two functions are the same when their instructions are equal with every branch target and immediate
ignored (registers kept), and they have the same length. Prints a C++ #if table for src/native/futari_native_core.cpp
(the native Direct3D layer's module table, tools/d3d_census_build.py's census).

  python tools/d3d_module_table.py NAME=8209D800 [NAME=820A8FC0 ...]
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'app', 'generated')
CORES = ['palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal']
FUNC = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?sub_([0-9A-F]{8})\)')


def loose(x):
    x = re.sub(r'0x[0-9a-f]{8}\b', 'T', x)
    return re.sub(r'(?<![rfv\w])-?\d+', 'N', x)


def functions(module):
    out, cur, body = {}, None, []
    for path in sorted(glob.glob(os.path.join(GEN, module, 'futari_recomp.*.cpp'))):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                if cur is not None:
                    out[cur] = tuple(body)
                cur, body = int(m.group(1), 16), []
                continue
            s = line.strip()
            if cur is not None and s.startswith('// '):
                body.append(loose(s[3:]))
            elif cur is not None and line.startswith('}'):
                out[cur] = tuple(body)
                cur, body = None, []
    if cur is not None:
        out[cur] = tuple(body)
    return out


def main():
    wanted = []
    for arg in sys.argv[1:]:
        name, _, addr = arg.partition('=')
        wanted.append((name, int(addr.replace('sub_', ''), 16)))
    default = functions('default')
    found = {name: {} for name, _ in wanted}
    for core in CORES:
        funcs = functions(core)
        by_shape = {}
        for a, body in funcs.items():
            by_shape.setdefault(body, []).append(a)
        for name, addr in wanted:
            found[name][core] = by_shape.get(default[addr], [])
    for name, addr in wanted:
        print('// %s: default.xex %08X (%d instructions)' % (name, addr, len(default[addr])))
        groups = {}
        for core in CORES:
            hits = found[name][core]
            key = hits[0] if len(hits) == 1 else None
            groups.setdefault(key, []).append(core)
        first = True
        for key, cores in groups.items():
            cond = ' || '.join('defined(FUTARI_CORE_%s)' % c for c in cores)
            if key is None:
                print('// NOT UNIQUE in %s: %s' % (', '.join(cores), [found[name][c] for c in cores]))
                continue
            print('#%s %s' % ('if' if first else 'elif', cond))
            print('#define FUTARI_CORE_%s %08X' % (name, key))
            first = False
        if not first:
            print('#endif')


if __name__ == '__main__':
    main()
