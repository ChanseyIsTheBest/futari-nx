#!/usr/bin/env python3
"""Renut's renderer (banjo-nx's copy in app/src/renut_engine/gpu) hooked into Futari's Direct3D by name (futari-nx,
2026-10-19; docs/futari-notes.md, "The deko3d renderer").

Renut's hooks name N&B's addresses (BANJO_RENUT_HOOK(sub_X, fn);  // Name). Futari's Direct3D is another XDK at other
addresses, and every game core links its own copy. This tool:

  1. rewrites the renderer's hook lines by name: FUTARI_RENUT_HOOK(Name, fn);  // N&B sub_X (once; idempotent);
  2. writes app/src/renut_engine/futari_renut_hooks.h: per name, its default.xex hook site (FUTARI_RENUT_SITE_Name,
     empty when Futari has no such function, app/src/renut_engine/futari_d3d_functions.txt, or when one of Futari's
     own hooks owns the address: that hook hands over with FUTARI_RENUT_YIELD(Name)), the entry declarations, and the
     cores' entry table (FUTARI_RENUT_CORE_ENTRIES: the names forwarded from the cores, in table order);
  3. writes app/src/renut_engine/futari_renut_core_hooks.cpp, compiled into every core target (FUTARI_CORE_<name>):
     the core's own copy of each mapped function (the same instructions as default.xex's, found below) forwards to the
     executable's entry while Renut's renderer is chosen. Addresses Futari's own core hooks own
     (src/native/futari_native_core.cpp) are left to them (FUTARI_RENUT_CORE_YIELD(Name)).

A core's copy: the function of the same length whose instructions are equal with branch targets ignored (exact tier;
device offsets kept), else equal with every immediate ignored (loose tier); unique in the core, and its direct calls
must land on the core's copies of what default.xex's calls (where those are mapped too).

  python tools/renut_futari_hooks.py [--gen DIR] [--check]
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
ENGINE = os.path.join(APP, 'src', 'renut_engine')
GPU = os.path.join(ENGINE, 'gpu')
MAP = os.path.join(ENGINE, 'futari_d3d_functions.txt')
OUT_H = os.path.join(ENGINE, 'futari_renut_hooks.h')
OUT_CORE = os.path.join(ENGINE, 'futari_renut_core_hooks.cpp')
CORE_SRC = os.path.join(APP, 'src', 'native', 'futari_native_core.cpp')
CORES = ['palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal']
FUNC = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?sub_([0-9A-F]{8})\)')

OLD_HOOK = re.compile(r'^(\s*)BANJO_RENUT_HOOK(?:_SHARED)?\((sub_[0-9A-Fa-f]{8}), (.+?)\);\s*//\s*(\w+)\s*$')
OLD_RAW = re.compile(r'^(\s*)BANJO_RENUT_HOOK_RAW(?:_SHARED)?\((sub_[0-9A-Fa-f]{8})\)\s*\{\s*(?://\s*(\w+))?\s*$')
NEW_HOOK = re.compile(r'^\s*FUTARI_RENUT_HOOK\((\w+), .+?\);')
NEW_RAW = re.compile(r'^\s*FUTARI_RENUT_HOOK_RAW\((\w+)\)')


def read_map():
    out = {}
    for line in open(MAP, encoding='utf-8'):
        f = line.split('#')[0].split()
        if len(f) >= 2:
            name, addr = f[0], int(f[1], 16)
            if name in out:
                sys.exit('futari_d3d_functions.txt: %s twice' % name)
            out[name] = addr
    addrs = {}
    for n, a in out.items():
        if a in addrs:
            sys.exit('futari_d3d_functions.txt: %08X is both %s and %s' % (a, addrs[a], n))
        addrs[a] = n
    return out


def renderer_sources():
    paths = []
    for ext in ('*.cpp', '*.h'):
        paths += glob.glob(os.path.join(GPU, '**', ext), recursive=True)
    return sorted(p for p in paths if os.path.basename(p) != 'banjo_renut_select.h')


def rewrite_sources(check):
    """The hook lines by name; returns {name: [file, ...]} of every hook in the renderer."""
    names = {}
    for path in renderer_sources():
        text = open(path, encoding='utf-8', newline='').read()
        eol = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(eol)
        changed = 0
        for i, line in enumerate(lines):
            m = OLD_HOOK.match(line)
            if m:
                ind, sub, fn, name = m.groups()
                lines[i] = '%sFUTARI_RENUT_HOOK(%s, %s);  // N&B %s' % (ind, name, fn, sub)
                changed += 1
            else:
                m = OLD_RAW.match(line)
                if m:
                    ind, sub, name = m.groups()
                    if not name:
                        sys.exit('%s:%d: a raw hook without its name in a comment' % (path, i + 1))
                    lines[i] = '%sFUTARI_RENUT_HOOK_RAW(%s) {  // N&B %s' % (ind, name, sub)
                    changed += 1
                elif re.search(r'\bBANJO_RENUT_(HOOK|YIELD)', line) and not line.lstrip().startswith('//'):
                    sys.exit('%s:%d: a hook line this tool does not know: %s' % (path, i + 1, line.strip()))
            m = NEW_HOOK.match(lines[i]) or NEW_RAW.match(lines[i])
            if m:
                names.setdefault(m.group(1), []).append(os.path.relpath(path, GPU).replace('\\', '/'))
        if changed:
            print('%s: %d hook lines by name' % (os.path.relpath(path, ROOT), changed))
            if not check:
                open(path, 'w', encoding='utf-8', newline='').write(eol.join(lines))
    return names


def strip_comments(text):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


def futari_owned_default():
    """default.xex addresses Futari's own hooks define (outside the renderer): {address: file}."""
    owned = {}
    for d, _, fs in os.walk(os.path.join(APP, 'src')):
        if os.path.abspath(d).startswith(os.path.abspath(ENGINE)):
            continue
        for f in fs:
            if not f.endswith(('.cpp', '.h')) or f == 'futari_native_core.cpp':
                continue
            path = os.path.join(d, f)
            text = strip_comments(open(path, encoding='utf-8', errors='ignore').read())
            for m in re.finditer(r'\b(?:REX_HOOK_RAW|REX_HOOK|REX_STUB)\(\s*sub_(82[0-9A-Fa-f]{6})\b', text):
                owned[int(m.group(1), 16)] = os.path.relpath(path, ROOT).replace('\\', '/')
            for m in re.finditer(r'extern\s+"C"\s+REX_FUNC\(\s*sub_(82[0-9A-Fa-f]{6})\)\s*\{', text):
                owned[int(m.group(1), 16)] = os.path.relpath(path, ROOT).replace('\\', '/')
    return owned


def futari_owned_cores():
    """Core addresses futari_native_core.cpp hooks, per core: the hex-only FUTARI_CORE_<X> defines (each is hooked
    there), under their #if defined(FUTARI_CORE_<core>) || ... conditions."""
    owned = {c: set() for c in CORES}
    current = []
    text = open(CORE_SRC, encoding='utf-8').read().replace('\\\r\n', ' ').replace('\\\n', ' ')
    for line in text.split('\n'):
        m = re.match(r'^#\s*(?:el)?if\b(.*)$', line)
        if m:
            current = [c for c in CORES if re.search(r'defined\(FUTARI_CORE_%s\)' % c, m.group(1))]
            continue
        if re.match(r'^#\s*(endif|else)\b', line):
            current = [] if line.lstrip('#').strip().startswith('endif') else current
            continue
        m = re.match(r'^#define FUTARI_CORE_\w+ (88[0-9A-F]{6})\s*$', line)
        if m:
            for c in current:
                owned[c].add(int(m.group(1), 16))
    return owned


def loose(x):
    x = re.sub(r'0x[0-9a-f]{8}\b', 'T', x)
    return re.sub(r'(?<![rfv\w])-?\d+', 'N', x)


def exact(x):
    return re.sub(r'0x[0-9a-f]{8}\b', 'T', x)


CALL = re.compile(r'^bl 0x([0-9a-f]{8})$')


def functions(gen, module):
    """{address: [instruction, ...]} of a module's generated code."""
    out, cur, body = {}, None, []
    for path in sorted(glob.glob(os.path.join(gen, module, '*_recomp.*.cpp'))):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = FUNC.match(line)
            if m:
                if cur is not None:
                    out[cur] = body
                cur, body = int(m.group(1), 16), []
                continue
            if cur is None:
                continue
            if line.startswith('\t// '):
                body.append(line[4:].rstrip())
            elif line.startswith('}'):
                out[cur] = body
                cur, body = None, []
    if cur is not None:
        out[cur] = body
    return out


def calls(body):
    return [int(m.group(1), 16) for m in (CALL.match(s) for s in body) if m]


def match_cores(gen, mapping):
    """{core: {name: core address}}, {core: {default.xex callee: the core's}} (from the matched functions' calls, in
    order), and the problems found."""
    default = functions(gen, 'default')
    result, callees, notes = {}, {}, []
    for core in CORES:
        funcs = functions(gen, core)
        by_exact, by_loose = {}, {}
        for a, body in funcs.items():
            by_exact.setdefault(tuple(exact(s) for s in body), []).append(a)
            by_loose.setdefault(tuple(loose(s) for s in body), []).append(a)
        found = {}
        for name, addr in mapping.items():
            body = default.get(addr)
            if body is None:
                notes.append('%s: %s %08X not in default.xex\'s generated code' % (core, name, addr))
                continue
            hits = by_exact.get(tuple(exact(s) for s in body), [])
            if len(hits) != 1:
                loose_hits = by_loose.get(tuple(loose(s) for s in body), [])
                if len(loose_hits) == 1 or not hits:
                    hits = loose_hits
            if len(hits) == 1:
                found[name] = hits[0]
            elif hits:
                notes.append('%s: %s not unique (%s)' % (core, name, ' '.join('%08X' % h for h in hits[:6])))
        # The call graph: default.xex's mapped caller -> mapped callee must be the core's caller -> callee.
        to_core = {mapping[n]: a for n, a in found.items()}
        for name in sorted(found):
            want = [to_core.get(c) for c in calls(default[mapping[name]])]
            got = calls(funcs[found[name]])
            bad = [(w, g) for w, g in zip(want, got) if w is not None and w != g]
            if bad or len(want) != len(got):
                notes.append('%s: %s at %08X calls other functions than default.xex\'s (dropped)' % (
                    core, name, found[name]))
                del found[name]
        pairs = {}
        for name, a in found.items():
            for w, g in zip(calls(default[mapping[name]]), calls(funcs[a])):
                pairs.setdefault(w, set()).add(g)
        callees[core] = {w: next(iter(g)) for w, g in pairs.items() if len(g) == 1}
        result[core] = found
    return result, callees, notes


HEADER = '''// Generated by tools/renut_futari_hooks.py from app/src/renut_engine/futari_d3d_functions.txt: do not edit.
// Renut's renderer (app/src/renut_engine/gpu, banjo-nx's copy of github.com/masterspike52/reNut by reNut and
// masterspike52) hooked into Futari's Direct3D by name (banjo_renut_select.h: FUTARI_RENUT_HOOK).
#pragma once

#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc.h>
'''


def write_header(names, mapping, owned, core_names):
    out = [HEADER]
    out.append('// Every hook\'s entry (defined by its FUTARI_RENUT_HOOK in the renderer).')
    for n in sorted(names):
        out.append('extern "C" void futari_renut_entry_%s(PPCContext& ctx, uint8_t* base);' % n)
    out.append('')
    out.append('// default.xex\'s hook sites: the game\'s function runs Renut\'s side while Renut\'s renderer is chosen.')
    for n in sorted(names):
        a = mapping.get(n)
        if a is None:
            out.append('#define FUTARI_RENUT_SITE_%s  // not in Futari\'s Direct3D' % n)
        elif a in owned:
            out.append('#define FUTARI_RENUT_SITE_%s  // sub_%08X: Futari\'s own hook (%s) yields' % (n, a, owned[a]))
        else:
            out.append('#define FUTARI_RENUT_SITE_%s \\' % n)
            out.append('  REX_EXTERN(__imp__sub_%08X); \\' % a)
            out.append('  extern "C" REX_FUNC(sub_%08X) { \\' % a)
            out.append('    if (::banjo::renut_gpu::Active()) { futari_renut_entry_%s(ctx, base); return; } \\' % n)
            out.append('    __imp__sub_%08X(ctx, base); \\' % a)
            out.append('  }')
    out.append('')
    out.append('// The cores\' forwarding table (futari_renut_core_hooks.cpp): index = position.')
    out.append('#define FUTARI_RENUT_CORE_ENTRIES(X) \\')
    for i, n in enumerate(core_names):
        out.append('  X(%s) /* %d */ \\' % (n, i))
    out.append('')
    for i, n in enumerate(core_names):
        out.append('#define FUTARI_RENUT_CORE_INDEX_%s %d' % (n, i))
    out.append('#define FUTARI_RENUT_CORE_COUNT %d' % len(core_names))
    out.append('')
    return '\n'.join(out)


CORE_HEAD = '''// Generated by tools/renut_futari_hooks.py: do not edit. Compiled into every game core (FUTARI_CORE_<name>):
// the core's own copies of the Direct3D functions Renut's renderer replaces (the same instructions as default.xex's)
// forward to the executable's entries while Renut's renderer is chosen (the executable's FutariRenutEntries():
// GetProcAddress on the PC, the final link on the Switch; null otherwise). Addresses futari_native_core.cpp hooks
// itself hand over there (FUTARI_RENUT_CORE_YIELD).
#if FUTARI_RENUT_RENDERER
#include "futari_renut_core.h"

'''


# The replacements that may free the game's own memory (d3d/hooks/resource.cpp GameXMemFree): in a core, through the
# core's XMemFree (default.xex's is sub_82088CE0; the core's is the callee in its place).
FREES = ('D3DResource_Destroy', 'D3DResource_Release')
DEFAULT_XMEMFREE = 0x82088CE0


def write_core(matched, callees, owned_cores, core_names, mapping, notes):
    out = [CORE_HEAD]
    for core in CORES:
        out.append('#if defined(FUTARI_CORE_%s)' % core)
        xmemfree = callees[core].get(DEFAULT_XMEMFREE)
        for n in core_names:
            a = matched[core].get(n)
            if a is None:
                out.append('// %s: no copy in this core' % n)
            elif a in owned_cores[core]:
                out.append('// %s: sub_%08X, futari_native_core.cpp\'s own hook yields' % (n, a))
            elif n in FREES:
                if xmemfree is None:
                    notes.append('%s: no XMemFree found for %s: not hooked' % (core, n))
                    out.append('// %s: sub_%08X, the core\'s XMemFree not found: not hooked' % (n, a))
                else:
                    out.append('FUTARI_RENUT_CORE_HOOK_FREE(%08X, %s, %08X)  // default.xex sub_%08X' % (
                        a, n, xmemfree, mapping[n]))
            else:
                out.append('FUTARI_RENUT_CORE_HOOK(%08X, %s)  // default.xex sub_%08X' % (a, n, mapping[n]))
        out.append('#endif')
        out.append('')
    out.append('#endif  // FUTARI_RENUT_RENDERER')
    out.append('')
    return '\n'.join(out)


def check_yields(names, mapping, owned, matched, owned_cores):
    problems = []
    for n in names:
        a = mapping.get(n)
        if a is not None and a in owned:
            path = os.path.join(ROOT, owned[a])
            text = open(path, encoding='utf-8').read()
            m = re.search(r'\b(?:REX_HOOK_RAW|REX_HOOK)\(\s*sub_%08X\b(.*?)\n\}' % a, text, re.S)
            if not m or 'FUTARI_RENUT_YIELD(%s)' % n not in m.group(1):
                problems.append('%s: the hook of sub_%08X has no FUTARI_RENUT_YIELD(%s)' % (owned[a], a, n))
    core_text = open(CORE_SRC, encoding='utf-8').read()
    for n in names:
        if any(matched[c].get(n) in owned_cores[c] for c in CORES if n in matched[c]):
            if 'FUTARI_RENUT_CORE_YIELD(%s)' % n not in core_text:
                problems.append('futari_native_core.cpp: the core hook of %s has no FUTARI_RENUT_CORE_YIELD(%s)' % (n, n))
    return problems


def main():
    check = '--check' in sys.argv
    gen = os.path.join(APP, 'generated')
    if '--gen' in sys.argv:
        gen = sys.argv[sys.argv.index('--gen') + 1]
    mapping = read_map()
    names = rewrite_sources(check)
    unknown = sorted(set(mapping) - set(names))
    if unknown:
        sys.exit('futari_d3d_functions.txt names no hook of the renderer: %s' % ', '.join(unknown))
    owned = futari_owned_default()
    owned_cores = futari_owned_cores()
    matched, callees, notes = match_cores(gen, mapping)
    # The names a core forwards: mapped, and found in at least one core.
    core_names = [n for n in sorted(mapping) if any(n in matched[c] for c in CORES)]
    header = write_header(names, mapping, owned, core_names)
    core = write_core(matched, callees, owned_cores, core_names, mapping, notes)
    for path, text in ((OUT_H, header), (OUT_CORE, core)):
        old = open(path, encoding='utf-8').read() if os.path.isfile(path) else None
        if old != text:
            print('%s %s' % ('would write' if check else 'wrote', os.path.relpath(path, ROOT)))
            if not check:
                open(path, 'w', encoding='utf-8', newline='\n').write(text)
    sites = sum(1 for n in names if n in mapping and mapping[n] not in owned)
    print('%d renderer hooks, %d mapped: %d default.xex sites, %d yield to Futari\'s own hooks' % (
        len(names), len(mapping), sites, sum(1 for n in mapping if mapping[n] in owned)))
    for c in CORES:
        hooked = sum(1 for n in core_names if n in matched[c] and matched[c][n] not in owned_cores[c])
        print('  %-16s %d of %d found, %d hooked here' % (c, len(matched[c]), len(mapping), hooked))
    for n in notes:
        print('  note: ' + n)
    problems = check_yields(names, mapping, owned, matched, owned_cores)
    for p in problems:
        print('  MISSING: ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
