#!/usr/bin/env python3
"""Functions that read a non-volatile register (r14-r31, f14-f31) their CALLER set, i.e. read it before writing it,
for anything other than saving it to the stack frame. With `non_volatile_as_local = true` each function keeps those
registers in C++ locals, so such a function would read garbage instead of the caller's value: it needs
`share_registers = true` in the codegen overrides (masseffect-nx needed five: a stack probe and four split-off
fragments; peggle2-nx hit one: RtlReAllocateHeap's split tail calling its __finally funclet with the caller's r31).

Runs over the BASELINE generated code (every register in ctx), function by function, in text order (a conservative
approximation of "before any write": a branch around the first write is treated as written). Saves are recognised as
stores of the register's own value into the frame (`REX_STORE_U64(ctx.r1.u32 + d, ctx.rN.u64)` and the 32-bit and FPR
forms); calls to the save/restore helpers (__savegprlr_N, __savefpr_N) count as saves too.

  python tools/scan_caller_register_reads.py [--gen out/generated_baseline] [--toml] [--with-callers]

--with-callers also marks every function that calls a reader directly (the parent of a split fragment or funclet keeps
the value in a C++ local otherwise, so the fragment's ctx read would still see a stale value: both sides need
share_registers).
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8}|__\w+)\)')
REG = re.compile(r'ctx\.([rf])(\d+)\b')
SAVE = re.compile(r'REX_STORE_(?:U64|U32|D_U64|D_U32)\(\s*ctx\.r1\.u32\s*(?:\+\s*-?\d+)?\s*,?\s*(?:-?\d+\s*,)?\s*ctx\.r(\d+)\.u(?:64|32)\s*\)')
SAVE_F = re.compile(r'REX_STORE_(?:U64|D_U64)\(.*ctx\.r1\.u32.*ctx\.f(\d+)\.u64')
ASSIGN = re.compile(r'^\s*ctx\.([rf])(\d+)\.(?:u64|s64|u32|s32|f64|u8|u16)\s*=(?!=)')
HELPER_CALL = re.compile(r'__(save|rest)(gpr|fpr)lr_(\d+)|__(save|rest)(gpr|fpr)_(\d+)')


def scan_function(name, lines):
    written = set()
    hits = {}
    for line in lines:
        s = line.strip()
        if not s or s.startswith('//'):
            continue
        m = HELPER_CALL.search(s)
        if m:
            # save helpers read the registers to store them: a save, not a use
            continue
        saved = {('r', int(x)) for x in SAVE.findall(s)} | {('f', int(x)) for x in SAVE_F.findall(s)}
        a = ASSIGN.match(s)
        lhs = (a.group(1), int(a.group(2))) if a else None
        rhs = s[a.end():] if a else s
        for kind, num in REG.findall(rhs):
            num = int(num)
            key = (kind, num)
            if num < 14 or key in written or key in saved or key == ('r', 1):
                continue
            hits.setdefault(key, s[:140])
        if lhs:
            written.add(lhs)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=os.path.join(ROOT, 'out', 'generated_baseline'))
    ap.add_argument('--toml', action='store_true', help='print [functions] share_registers lines per module')
    ap.add_argument('--with-callers', action='store_true', help='also mark the direct callers of each reader')
    a = ap.parse_args()
    for folder in sorted(glob.glob(os.path.join(a.gen, '*', ''))):
        module = os.path.basename(folder.rstrip('/\\'))
        found = {}
        for path in sorted(glob.glob(os.path.join(folder, '*_recomp.*.cpp'))):
            name, body = None, []
            for line in open(path, encoding='utf-8', errors='replace'):
                m = FUNC.match(line)
                if m:
                    if name and not name.startswith('__'):
                        h = scan_function(name, body)
                        if h:
                            found[name] = h
                    name, body = m.group(1), []
                elif name:
                    body.append(line)
            if name and not name.startswith('__'):
                h = scan_function(name, body)
                if h:
                    found[name] = h
        callers = {}
        if a.with_callers:
            call = re.compile(r'(?<![\w])(?:__imp__)?(sub_[0-9A-F]{8})\(ctx')
            for path in sorted(glob.glob(os.path.join(folder, '*_recomp.*.cpp'))):
                current = None
                for line in open(path, encoding='utf-8', errors='replace'):
                    m = FUNC.match(line)
                    if m:
                        current = m.group(1)
                        continue
                    for target in call.findall(line):
                        if target in found and target != current:
                            callers.setdefault(current, set()).add(target)
        print(f'== {module}: {len(found)} functions read a caller non-volatile register before writing it'
              + (f', {len(set(callers) - set(found))} more call one' if a.with_callers else ''))
        for fn, h in sorted(found.items()):
            regs = ', '.join(f'{k}{n}' for (k, n) in sorted(h))
            first = next(iter(h.values()))
            if a.toml:
                print(f'"0x{fn[4:]}" = {{ share_registers = true }}   # reads {regs}: {first[:80]}')
            else:
                print(f'  {fn}: {regs} | {first}')
        for fn in sorted(set(callers) - set(found)):
            readers = ', '.join(sorted(callers[fn]))
            if a.toml:
                print(f'"0x{fn[4:]}" = {{ share_registers = true }}   # calls {readers}')
            else:
                print(f'  {fn}: calls {readers}')


if __name__ == '__main__':
    main()
