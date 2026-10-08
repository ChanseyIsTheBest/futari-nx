#!/usr/bin/env python3
"""Checks a Switch build before it is packaged (sa2-nx's tools/check_switch_build.py, from sasrt-nx and nfsmw-nx).

  python tools/check_switch_build.py [--elf out/switch/futari] [--nro out/switch/futari.nro]

Checks (exit 1 if any fails):
  - _start is at address 0 (nfsmw-nx: a section-ordering file that moved the start-up code made the NRO unbootable;
    app/hot_functions.ld starts with KEEP(*(.crt0)) for that reason);
  - the NRO links the nfsmw NVK driver of the SDK (its "Mesa x.y.z" string; the generic portlibs Mesa once got in);
  - no vector fmla/fmls in recompiled functions: the generated code has no vector fused multiply-add (VMX
    multiply-adds are a multiply and an add), so any would be contraction that leaked past -ffp-contract=off (e.g. at
    the LTO link). Scalar fmadd/fmsub/fnmadd/fnmsub come from the guest's own std::fma and are only counted.
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
    sdk = sorted(set(re.findall(rb'Mesa \d+\.\d+\.\d+ \(git-[0-9a-f]+\)', open(a.sdk_vulkan, 'rb').read())))
    print('Mesa in the NRO:', [m.decode() for m in mesa], '| in the SDK:', [m.decode() for m in sdk])
    if not mesa or mesa != sdk:
        failures.append('the NRO does not carry the SDK driver\'s Mesa string')

    # One streaming pass over the disassembly: fused multiply-adds per recompiled function.
    proc = subprocess.Popen([DEVKITA64 + 'aarch64-none-elf-objdump' + EXE, '-d', '--no-show-raw-insn', a.elf],
                            stdout=subprocess.PIPE, text=True, errors='replace', bufsize=1 << 20)
    func = None
    vector_fma = collections.Counter()
    scalar_fma = 0
    head = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
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
        guest = func.startswith(('sub_8', '__imp__sub_8'))
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
    if failures:
        print('\nFAILED:\n  - ' + '\n  - '.join(failures))
        sys.exit(1)
    print('\nall checks passed')


if __name__ == '__main__':
    main()
