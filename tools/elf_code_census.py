#!/usr/bin/env python3
"""Instruction counts of the recompiled game code in a Switch ELF (futari-nx, 2026-10-19; for comparing two builds
of the generated code, e.g. before and after banjo-nx's codegen round). One streaming pass over objdump -d: per
function named sub_XXXXXXXX or __imp__sub_XXXXXXXX (both modules' kinds: default.xex 82..., the game cores 88...),
and the localized copies (futari::hot), the totals of instructions, loads, stores, stack loads / stores (sp-relative),
calls (bl / blr), byte reverses (rev, rev16, rev32), msr fpcr and table lookups (tbl). Static counts.

  python tools/elf_code_census.py <elf> [<elf> ...]
"""
import collections
import os
import re
import subprocess
import sys

DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
OBJDUMP = DEVKITA64 + 'aarch64-none-elf-objdump' + ('.exe' if os.name == 'nt' else '')
HEAD = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
INS = re.compile(r'^\s+[0-9a-f]+:\t(\S+)(?:\t(.*))?$')
GAME = re.compile(r'^(?:__imp__)?sub_[0-9A-F]{8}(?:\.\w+)*$')
KEYS = ['functions', 'instructions', 'loads', 'stores', 'stack loads', 'stack stores', 'calls', 'byte reverses',
        'msr fpcr', 'tbl']


def census(elf):
    totals = {'game': collections.Counter(), 'copies': collections.Counter()}
    p = subprocess.Popen([OBJDUMP, '-d', '--no-show-raw-insn', elf], stdout=subprocess.PIPE, text=True,
                         encoding='utf-8', errors='replace', bufsize=1 << 20)
    kind = None
    for line in p.stdout:
        m = HEAD.match(line.rstrip('\n'))
        if m:
            name = m.group(1)
            kind = 'game' if GAME.match(name) else 'copies' if name.startswith('_ZN6futari3hot') else None
            if kind:
                totals[kind]['functions'] += 1
            continue
        if not kind:
            continue
        m = INS.match(line)
        if not m:
            continue
        mn, ops = m.group(1), m.group(2) or ''
        t = totals[kind]
        t['instructions'] += 1
        if mn.startswith('ld'):
            t['loads'] += 1
            if '[sp' in ops:
                t['stack loads'] += 1
        elif mn.startswith('st'):
            t['stores'] += 1
            if '[sp' in ops:
                t['stack stores'] += 1
        elif mn in ('bl', 'blr'):
            t['calls'] += 1
        elif mn in ('rev', 'rev16', 'rev32'):
            t['byte reverses'] += 1
        elif mn == 'msr' and ops.startswith('fpcr'):
            t['msr fpcr'] += 1
        elif mn == 'tbl':
            t['tbl'] += 1
    p.wait()
    return totals


def main():
    results = [(elf, census(elf)) for elf in sys.argv[1:]]
    for kind in ('game', 'copies'):
        print('%s:' % ('recompiled game functions' if kind == 'game' else 'localized copies'))
        print('  %-14s' % '' + ''.join('%18s' % os.path.basename(os.path.dirname(os.path.abspath(e))) for e, _ in results))
        for k in KEYS:
            row = [r[kind][k] for _, r in results]
            delta = '' if len(row) < 2 or not row[0] else '   %+.1f %%' % (100.0 * (row[-1] - row[0]) / row[0])
            print('  %-14s' % k + ''.join('%18s' % '{:,}'.format(v) for v in row) + delta)


if __name__ == '__main__':
    main()
