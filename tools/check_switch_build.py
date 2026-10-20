#!/usr/bin/env python3
"""Checks a Switch build before it is packaged (sa2-nx's tools/check_switch_build.py, from sasrt-nx and nfsmw-nx).

  python tools/check_switch_build.py [--elf out/switch/futari] [--nro out/switch/futari.nro]

Checks (exit 1 if any fails):
  - _start is at address 0 (nfsmw-nx: a section-ordering file that moved the start-up code made the NRO unbootable;
    app/hot_functions.ld starts with KEEP(*(.crt0)) for that reason);
  - the NRO links the nfsmw NVK driver of the SDK (its "Mesa x.y.z" string; the generic portlibs Mesa once got in),
    or, built with FUTARI_SWITCH_NVK=OFF (the default since 2026-10-20: deko3d only), carries no Mesa at all;
  - no vector fmla/fmls in recompiled functions: the generated code has no vector fused multiply-add (VMX
    multiply-adds are a multiply and an add), so any would be contraction that leaked past -ffp-contract=off (e.g. at
    the LTO link). Scalar fmadd/fmsub/fnmadd/fnmsub come from the guest's own std::fma and are only counted.
  - no table lookup indexed by its own table (`tbl vA, {vB}, vB`, or the index among the registers of a two-register
    table) in recompiled code: the symptom of devkitA64 GCC 16.1's early register allocation pass dropping the
    lookups of the NEON vperm helper and overwriting a register (peggle2-nx, 2026-10-13; the build sets
    -mearly-ra=none, app/CMakeLists.txt). The lookups are counted;
  - no fcvtas (float to integer, halves away from zero) in recompiled code: fctid / fctiw round in the current mode
    (rex::ppc::fctid, generator option fcti_inline; simde's portable conversion was round() until 1.0.0).
Branch-to-self loops are tools/find_self_loops.py's check (run it too).
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--elf', default=os.path.join(ROOT, 'out', 'switch', 'futari'))
    ap.add_argument('--nro', default=os.path.join(ROOT, 'out', 'switch', 'futari.nro'))
    ap.add_argument('--sdk-vulkan', default='C:/devkitPro/mesa-switch-main-nfsmw-source/mesa-switch-main/'
                    'mesa-unified-install/opt/devkitpro/portlibs/switch/lib/libvulkan.a')
    a = ap.parse_args()
    failures = []

    nm = subprocess.run([DEVKITA64 + 'aarch64-none-elf-nm' + EXE, a.elf], capture_output=True, text=True).stdout
    start = next((int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' _start')), None)
    print(f'_start at {start:#x}' if start is not None else '_start not found')
    if start != 0:
        failures.append('_start is not at address 0')

    nro = open(a.nro, 'rb').read()
    mesa = sorted(set(re.findall(rb'Mesa \d+\.\d+\.\d+ \(git-[0-9a-f]+\)', nro)))
    # FUTARI_SWITCH_NVK (app/CMakeLists.txt) from the build's cache: OFF links no driver (sdk/src/ui/switch_vulkan_stub.c).
    nvk = True
    cache = os.path.join(os.path.dirname(os.path.abspath(a.elf)), 'CMakeCache.txt')
    if os.path.exists(cache):
        m = re.search(r'^FUTARI_SWITCH_NVK:BOOL=(\w+)', open(cache, encoding='utf-8', errors='replace').read(), re.M)
        if m:
            nvk = m.group(1).upper() in ('ON', 'TRUE', '1', 'YES')
    if nvk:
        sdk = sorted(set(re.findall(rb'Mesa \d+\.\d+\.\d+ \(git-[0-9a-f]+\)', open(a.sdk_vulkan, 'rb').read())))
        print('Mesa in the NRO:', [m.decode() for m in mesa], '| in the SDK:', [m.decode() for m in sdk])
        if not mesa or mesa != sdk:
            failures.append('the NRO does not carry the SDK driver\'s Mesa string')
    else:
        print('Mesa in the NRO:', [m.decode() for m in mesa], '(FUTARI_SWITCH_NVK=OFF: none expected)')
        if mesa:
            failures.append('FUTARI_SWITCH_NVK is OFF but the NRO carries a Mesa driver')

    # One streaming pass over the disassembly: fused multiply-adds per recompiled function.
    proc = subprocess.Popen([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', a.elf],
                            stdout=subprocess.PIPE, text=True, errors='replace', bufsize=1 << 20)
    func = None
    vector_fma = collections.Counter()
    scalar_fma = 0
    lookups = guest_lookups = two_register = 0
    self_indexed = collections.Counter()
    away = collections.Counter()
    vreg = re.compile(r'v(\d+)\.16b')
    head =re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
    ins = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$')
    for line in proc.stdout:
        m = head.match(line)
        if m:
            func = m.group(1)
            continue
        m = ins.match(line)
        if not m or not func:
            continue
        op = m.group(2)
        guest = func.startswith(('sub_8', '__imp__sub_8')) or '6futari3hot' in func
        if op in ('tbl', 'tbx'):
            lookups += 1
            guest_lookups += guest
            # tbl vD, {vT[, vT2 | -vT2]}, vI
            operands = m.group(3)
            table = operands[operands.find('{') + 1:operands.find('}')]
            registers = [int(r) for r in vreg.findall(table)]
            if '-' in table and len(registers) == 2:
                first, last = registers
                registers = [(first + k) % 32 for k in range((last - first) % 32 + 1)]
            two_register += len(registers) > 1
            index = vreg.findall(operands[operands.find('}') + 1:])
            if index and int(index[-1]) in registers:
                self_indexed[func] += 1
        elif op == 'fcvtas' and guest:
            away[func] += 1
        if op in ('fmla', 'fmls') and guest:
            vector_fma[func] += 1
        elif op in ('fmadd', 'fmsub', 'fnmadd', 'fnmsub') and guest:
            scalar_fma += 1
    proc.wait()
    print(f'fused multiply-adds in recompiled functions: scalar {scalar_fma} (the guest\'s own), vector '
          f'{sum(vector_fma.values())} in {len(vector_fma)} functions (must be 0)')
    if vector_fma:
        failures.append('vector fmla/fmls in recompiled code (contraction leaked): '
                        + ', '.join(f'{k} x{v}' for k, v in vector_fma.most_common(10)))
    print(f'table lookups: {lookups} ({guest_lookups} in recompiled functions, {two_register} with a table of two or '
          f'more registers), indexed by their own table: {sum(self_indexed.values())} (must be 0)')
    if self_indexed:
        failures.append('table lookups indexed by their own table (GCC early-ra miscompile?): '
                        + ', '.join(f'{k} x{v}' for k, v in self_indexed.most_common(10)))
    print(f'fcvtas in recompiled functions: {sum(away.values())} in {len(away)} functions (must be 0)')
    if away:
        failures.append('fcvtas (halves away from zero) in recompiled code: '
                        + ', '.join(f'{k} x{v}' for k, v in away.most_common(10)))
    if failures:
        print('\nFAILED:\n  - ' + '\n  - '.join(failures))
        sys.exit(1)
    print('\nall checks passed')


if __name__ == '__main__':
    main()
