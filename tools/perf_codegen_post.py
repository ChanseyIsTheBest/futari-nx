#!/usr/bin/env python3
"""Post-codegen performance passes of masseffect-nx (tools/direct_calls.py, tools/pch_no_volatile.py there), for every
generated folder of Futari (default.xex and the eight game cores). Run after EVERY codegen (tools/codegen.ps1 -Perf
does); both passes are idempotent and refuse to run on a generated tree whose shape changed.

1. Direct calls. Every recompiled sub_X is a weak alias of __imp__sub_X so a REX_HOOK can replace it at link time, and
   the codegen always calls sub_X: GCC cannot inline a weak function. `sub_X(ctx, base);` becomes
   `__imp__sub_X(ctx, base);` when sub_X has no hook. Any 82xxxxxx or 88xxxxxx address that appears in the app or SDK
   sources or in the tomls counts as hooked (slightly more than needed, and safe). The dispatch tables
   (futari_init/register) are not touched, so indirect calls still see the hooks. masseffect-nx t119.
2. Guest loads and stores without `volatile` (REX_LOAD_*/REX_STORE_* in futari_pch.h); MMIO macros keep it.
   masseffect-nx t153: +15-20 %. Risk: a loop that only polls guest memory could be hoisted (none seen there).
   Define REX_GUEST_VOLATILE as volatile to get the old behaviour back without a codegen.
3. Loop barriers (REQUIRED with 2). GCC (the Switch build) DID hoist guest polling loads out of loops: 172 functions
   became branch-to-self infinite loops (tools/find_self_loops.py), e.g. D3D's `while (device[11012] != 0) {}` in
   sub_820A9268, which hung the boot on the console (2026-10-06; clang on the PC had not done it). Every loop header
   (a label that a later goto of the same function jumps back to) gets REX_LOOP_BARRIER(), a compiler memory barrier:
   no guest load is reused across iterations, including loads of callees inlined into the loop (direct calls make
   that possible). Straight-line code keeps the plain loads.

  python tools/perf_codegen_post.py [--undo] [--check] [--no-direct] [--no-volatile] [--no-barriers]
                                    [--hooked-list FILE] [--gen DIR]

--hooked-list FILE: more hooked functions, one sub_XXXXXXXX (or address) per line, for hooks outside the scanned sources
(a Direct3D census build: out/census/hooks.txt, tools/d3d_census_build.py). Running the pass again without the list
makes those calls direct again.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
SDK = os.path.join(ROOT, 'sdk')
GEN = os.path.join(APP, 'generated')
SOURCES = [os.path.join(APP, 'src'), os.path.join(SDK, 'src'), os.path.join(SDK, 'include'), os.path.join(APP, 'CMakeLists.txt')]
SOURCES += [os.path.join(APP, f) for f in os.listdir(APP) if f.endswith('.toml')]


# Text that names functions without hooking them (sa2-nx's round-4 lesson: hook descriptions that named other
# functions' addresses kept 264 call sites indirect; here hot_functions.txt and the hot copies' record declarations kept
# memset's 435): data files, comments, and FUTARI_HOT_RECORD_DECL / #include lines are not scanned.
COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)
IMP = re.compile(r'__imp__sub_[0-9A-Fa-f]{8}')
NOT_A_HOOK = re.compile(r'FUTARI_HOT_RECORD_DECL\([0-9A-Fa-f]{8}\)|#include[^\n]*')


def hash_comments_out(text):
    """A toml's or CMakeLists.txt's text without its `#` comments (banjo-nx, 2026-10-19: 24 of its functions were on the
    weak alias only because comments named them). A `#` inside a double-quoted string is kept."""
    out = []
    for line in text.split('\n'):
        quoted = False
        for k, c in enumerate(line):
            if c == '"':
                quoted = not quoted
            elif c == '#' and not quoted:
                line = line[:k]
                break
        out.append(line)
    return '\n'.join(out)


def hooked():
    names = set()
    for root in SOURCES:
        paths = [root] if os.path.isfile(root) else [
            os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs
            if not f.endswith(('.a', '.obj', '.o', '.png', '.jpg', '.bin', '.txt', '.md'))]
        for path in paths:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            if path.endswith(('.c', '.cc', '.cpp', '.h', '.hpp', '.inl')):
                text = NOT_A_HOOK.sub('', COMMENT.sub('', text))
            elif path.endswith(('.toml', 'CMakeLists.txt')):
                text = hash_comments_out(text)
            # A call of the original (__imp__sub_X: a native layer calling Direct3D's own helper) is not a hook.
            text = IMP.sub('', text)
            for m in re.findall(r'(?<![0-9A-Fa-f])(8[28][0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text):
                names.add('sub_' + m.upper())
    return names


def direct_calls(folder, with_hook, undo, check):
    call = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    changed = files = seen = already = rehooked = 0
    for f in sorted(os.listdir(folder)):
        if not (f.startswith('futari_recomp.') and f.endswith('.cpp')):
            continue
        path = os.path.join(folder, f)
        text = open(path, encoding='utf-8').read()
        seen += 1
        already += len(direct.findall(text))
        if undo:
            new, n = direct.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), text)
        else:
            # A hook added after an earlier run (a native replacement in app/src/native) turns its direct calls back
            # into calls of the hookable name; the codegen itself never writes __imp__ calls.
            text_hooked = direct.sub(lambda m: 'sub_%s(ctx, base);' % m.group(1) if 'sub_' + m.group(1) in with_hook
                                     else m.group(0), text)
            new = call.sub(lambda m: m.group(0) if 'sub_' + m.group(1) in with_hook
                           else '__imp__sub_%s(ctx, base);' % m.group(1), text_hooked)
            n = len(call.findall(text_hooked)) - len(call.findall(new))
            rehooked += len(direct.findall(text)) - len(direct.findall(text_hooked))
        if new != text:
            files += 1
            if not check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
        changed += n
    if seen == 0:
        sys.exit('direct calls: no futari_recomp.*.cpp in ' + folder)
    if not undo and changed == 0 and already == 0:
        sys.exit('direct calls: nothing to convert and no direct calls in %s: the generated call shape changed' % folder)
    return changed, files, already, rehooked


def no_volatile(folder, undo, check):
    path = os.path.join(folder, 'futari_pch.h')
    s = open(path, encoding='utf-8').read()
    marker = '// futari-nx: guest loads/stores without volatile'
    if undo:
        if marker not in s:
            return 'already volatile'
        s = s.replace('(REX_GUEST_VOLATILE u', '(volatile u')
        start = s.index(marker)
        end = s.index('#define REX_LOAD_U8(x)', start)
        s = s[:start] + s[end:]
        if not check:
            open(path, 'w', encoding='utf-8', newline='\n').write(s)
        return 'volatile restored'
    if 'REX_GUEST_VOLATILE' in s:
        return 'already plain'
    out, changed = [], 0
    for line in s.split('\n'):
        if line.startswith(('#define REX_LOAD_U', '#define REX_STORE_U', '#define REX_LOAD_D_U', '#define REX_STORE_D_U')):
            n = line.replace('(volatile u', '(REX_GUEST_VOLATILE u')
            changed += n != line
            line = n
        out.append(line)
    if changed == 0 or '#define REX_LOAD_U8(x)' not in s:
        sys.exit('no volatile: no REX_LOAD_*/REX_STORE_* "(volatile u" macros in %s: the generated pch changed' % path)
    s = '\n'.join(out).replace('#define REX_LOAD_U8(x)', marker + ''' (tools/perf_codegen_post.py, masseffect-nx
// t153). Define REX_GUEST_VOLATILE as volatile to restore the generated behaviour. MMIO macros keep volatile.
#ifndef REX_GUEST_VOLATILE
#define REX_GUEST_VOLATILE
#endif
#define REX_LOAD_U8(x)''', 1)
    if not check:
        open(path, 'w', encoding='utf-8', newline='\n').write(s)
    return '%d macros plain' % changed


LABEL = re.compile(r'^(loc_[0-9A-F]{8}):\s*$')
GOTO = re.compile(r'goto (loc_[0-9A-F]{8});')
BARRIER = '\tREX_LOOP_BARRIER();'
BARRIER_MARKER = '// futari-nx: loop barrier (tools/perf_codegen_post.py pass 3)'
BARRIER_MACRO = (BARRIER_MARKER + '\n#ifndef REX_LOOP_BARRIER\n'
                 '#define REX_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")\n#endif\n')


def loop_barriers(folder, undo, check):
    headers_total = files = 0
    for f in sorted(os.listdir(folder)):
        if not (f.startswith('futari_recomp.') and f.endswith('.cpp')):
            continue
        path = os.path.join(folder, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        if undo:
            out = [l for l in lines if l != BARRIER]
        else:
            # Loop headers: per function, labels that a goto after them jumps back to.
            headers, seen = set(), set()
            for line in lines:
                if line.startswith('DEFINE_REX_FUNC('):
                    seen = set()
                    continue
                m = LABEL.match(line)
                if m:
                    seen.add(m.group(1))
                    continue
                for target in GOTO.findall(line):
                    if target in seen:
                        headers.add(target)
            out = []
            for i, line in enumerate(lines):
                out.append(line)
                m = LABEL.match(line)
                if m and m.group(1) in headers and (i + 1 >= len(lines) or lines[i + 1] != BARRIER):
                    out.append(BARRIER)
            headers_total += len(headers)
        if out != lines:
            files += 1
            if not check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    # The macro, in the precompiled header (before the guest memory macros).
    pch = os.path.join(folder, 'futari_pch.h')
    text = open(pch, encoding='utf-8').read()
    if not undo and BARRIER_MARKER not in text:
        if '#define REX_LOAD_U8(x)' not in text:
            sys.exit('loop barriers: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        text = text.replace('#define REX_LOAD_U8(x)', BARRIER_MACRO + '#define REX_LOAD_U8(x)', 1)
        if not check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text)
    return '%s %d loop headers in %d files' % ('barriers removed in' if undo else 'barriers at', headers_total, files)


def main():
    global GEN
    if '--gen' in sys.argv:  # another generated tree (a scratch copy for measurements: tools/post_codegen.py --gen)
        GEN = sys.argv[sys.argv.index('--gen') + 1]
    undo = '--undo' in sys.argv
    check = '--check' in sys.argv
    with_hook = hooked()
    if '--hooked-list' in sys.argv:
        path = sys.argv[sys.argv.index('--hooked-list') + 1]
        for line in open(path, encoding='utf-8'):
            m = re.search(r'([0-9A-Fa-f]{8})', line)
            if m:
                with_hook.add('sub_' + m.group(1).upper())
    folders = sorted(os.path.join(GEN, d) for d in os.listdir(GEN) if os.path.isfile(os.path.join(GEN, d, 'futari_pch.h')))
    if not folders:
        sys.exit('no generated folders under ' + GEN)
    for folder in folders:
        parts = []
        if '--no-direct' not in sys.argv:
            n, files, already, rehooked = direct_calls(folder, with_hook, undo, check)
            parts.append('%s %d calls in %d files (%d direct before%s)' % (
                'undone' if undo else 'direct', n, files, already,
                ', %d back to their hook' % rehooked if rehooked else ''))
        if '--no-volatile' not in sys.argv:
            parts.append(no_volatile(folder, undo, check))
            # --no-barriers: tools/loop_barriers.py places them (tools/post_codegen.py; sa2-nx's refinement: no
            # barrier where it has nothing to protect).
            if '--no-barriers' not in sys.argv:
                parts.append(loop_barriers(folder, undo, check))
        print('%-16s %s' % (os.path.basename(folder), '; '.join(parts)))
    print('%d hooked addresses respected%s' % (len(with_hook), ' (check only: nothing written)' if check else ''))


if __name__ == '__main__':
    main()
