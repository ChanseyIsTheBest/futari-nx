#!/usr/bin/env python3
"""Writes app/hot_functions.ld: the functions that ran most on the console grouped at the start of .text (passed to
the Switch link with -Wl,--section-ordering-file when FUTARI_SWITCH_HOT_ORDER is on; sa2-nx's tools/hot_order.py,
from nfsmw-nx's orden_funciones.ld).

Why: the NRO has ~55 MB of code and the Cortex-A57 has a 48 KB instruction cache and a small TLB. Hot functions
scattered across the whole image cost instruction-cache and TLB misses on every far call; grouped, the hot set spans a
few hundred KB. Only placement changes (every function already has its own section, -ffunction-sections).

Input: one or more console profiles (logs/rex/rex_profile.log, switch_profile_stacks = true) and the link map of the
build that recorded each (futari.map, kept in out/switch_ab/<build>/). Every sampled address (pc, lr and the stack
frames) adds the share of samples it appeared in to its function, over every thread and interval. Futari differences
from sa2-nx's tool:
  - the map gives each address's input section (and object) directly, so no ELF is needed;
  - the eight game cores (rexglue_static_modules/futari_<core>.o) have the same function names (.text.__imp__sub_88...):
    their entries name the core's object, so a hot function of one core does not drag the seven other copies along;
  - profiles recorded before the 2026-10-08 rename name the renderer masseffect::native: those names are translated
    (futari::renderer); a name that no longer exists simply matches nothing.

  python tools/hot_order.py --profile <rex_profile.log> <futari.map> [--profile <log2> <map2> ...] [--count 1500]

THE TRAP (nfsmw-nx): the linker places what this file lists BEFORE everything in its own script, including libnx's
start-up code. The first line inside .text must be KEEP (*(.crt0)), and every build is checked for _start at 0
(tools/check_switch_build.py).
"""
import argparse
import bisect
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from switch_hot_functions import load_map  # noqa: E402  (the .text.* input sections of a link map)

# Assembly routines of newlib with no per-function section: named by archive member (nfsmw-nx's list).
LIBC_OBJECTS = ['memcpy', 'memmove', 'memset', 'memcmp', 'strcmp', 'strlen']
# A sleeping thread's stack starts in the caller of the system call (the svc stubs keep no frame record): those callers
# are waits too (libnx events, audout, condition variables, futex-style waits).
WAITING = re.compile(r'__syscall_|^svc|nanosleep|cond_wait|cond_timedwait|condvarWait|WaitProcessWideKey|'
                     r'WaitSynchronization|SleepThread|eventWait|leventWait|WaitPlayFinish|waitSingle|waitObjects|'
                     r'waitMulti|_waitLoop|WaitFor|semaphoreWait|mutexLock|rwlock')
CORE = re.compile(r'(futari_(?:palm|reco|hirow|kiniro)_(?:normal|abnormal))\.o$')


def translate(name):
    """A section name from before the rename (masseffect -> futari, masseffect::native -> futari::renderer)."""
    if 'masseffect' not in name:
        return name
    out, i, previous = [], 0, ''
    while i < len(name):
        m = re.match(r'(\d+)', name[i:])
        if m and name[i - 1:i] not in '0123456789':
            n = int(m.group(1))
            start = i + len(m.group(1))
            ident = name[start:start + n]
            if len(ident) == n and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', ident):
                new = ident.replace('masseffect', 'futari')
                if previous == 'masseffect' and ident == 'native':
                    new = 'renderer'
                out.append(str(len(new)) + new)
                previous = ident
                i = start + n
                continue
        out.append(name[i])
        i += 1
    return ''.join(out).replace('masseffect', 'futari')


def weigh(log, mapfile, weight):
    sections = load_map(mapfile)
    starts = [s[0] for s in sections]
    samples = 0
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.match(r'\s+(\d+) samples, ', line)
        if m:
            samples = int(m.group(1))
            continue
        m = re.match(r'\s+([\d.]+)%\s+(pc|lr|stack|wait) (.*)$', line)
        if not m or m.group(2) == 'wait':
            continue
        if m.group(2) == 'pc' and re.search(r' x\d+\s*$', line):
            share = float(re.search(r' x(\d+)\s*$', line).group(1))
        else:
            share = float(m.group(1)) * samples / 100.0
        frames = []
        for a in re.findall(r'image\+0x([0-9a-f]+)', m.group(3)):
            pc = int(a, 16)
            i = bisect.bisect_right(starts, pc) - 1
            if i >= 0 and pc < sections[i][1]:
                core = CORE.search(sections[i][3].replace('\\', '/'))
                frames.append((translate(sections[i][2]), core.group(1) if core else ''))
            else:
                frames.append(('??', ''))
        # A stack whose innermost frame is a kernel wait is a sleeping thread: not hot code.
        if m.group(2) == 'stack' and frames and WAITING.search(frames[0][0]):
            continue
        for key in set(frames):
            weight[key] += share


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--profile', nargs=2, action='append', metavar=('LOG', 'MAP'), required=True)
    ap.add_argument('--count', type=int, default=1500)
    ap.add_argument('--out', default=os.path.join(ROOT, 'app', 'hot_functions.ld'))
    a = ap.parse_args()
    weight = collections.Counter()
    for log, mapfile in a.profile:
        weigh(log, mapfile, weight)
    # Functions without their own section or that must not move: the start-up code is placed first explicitly.
    for key in list(weight):
        if key[0] in ('_start', '__libnx_init', '??') or key[0] in LIBC_OBJECTS:
            del weight[key]
    hot = [k for k, _ in weight.most_common(a.count)]
    lines = ['/*',
             f' * Hot functions grouped at the start of .text ({len(hot)} of them, hottest first), from console stack',
             ' * sampling. Generated by tools/hot_order.py from ' + ', '.join(
                 os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(p[0])))) or p[0] for p in a.profile)
             + '.',
             ' * Each entry also matches .text.hot.NAME: a PGO build (FUTARI_PGO=use) puts the functions its profile found hot',
             ' * there (223 of these in the first PGO build). The start-up code must stay first (nfsmw-nx: memcpy at address 0',
             ' * made the NRO unbootable).',
             ' */',
             '.text : {',
             '  KEEP (*(.crt0))',
             '  /* newlib assembly routines without their own section: by archive member */']
    lines += [f'  *libc.a:libc_a-{n}.o(.text .text.*)' for n in LIBC_OBJECTS]
    # Each name also matches its compiler clones (.text.NAME.*): with LTO, functions of an anonymous namespace become
    # NAME.lto_priv.N, and .constprop / .isra / .part clones of a profile from another build may be numbered otherwise
    # (the first LTO build placed only 1417 of 1500: the misses were the renderer's hottest, Draw, Type3, Packet...).
    seen = set()
    for name, core in hot:
        base = re.sub(r'(\.(lto_priv|constprop|isra|part|cold)\.\d+)+$', '', name)
        if (base, core) in seen:
            continue
        seen.add((base, core))
        # GCC with a profile (FUTARI_PGO=use) names the functions it finds hot .text.hot.NAME: those must match too.
        pattern = f'.text.{base} .text.{base}.* .text.hot.{base} .text.hot.{base}.*'
        lines.append(f'  *{core}.o({pattern})' if core else f'  *({pattern})')
    lines.append('}')
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    guest = sum(1 for n, c in hot if '__imp__sub_82' in n)
    cores = collections.Counter(c for n, c in hot if c)
    print(f'{a.out}: {len(hot)} functions ({guest} of default.xex, game cores {dict(cores)}); hottest: '
          f'{[n for n, c in hot[:6]]}')


if __name__ == '__main__':
    main()
