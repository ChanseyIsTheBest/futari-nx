#!/usr/bin/env python3
"""Localized copies of recompiled guest functions (sonic adventure 2's tool, after UnleashedRecomp-NX's
switch-localize.py), with a second copy for the record-and-replay guard.

A localized copy runs exactly the recompiled function's own C++ statements, with the guest registers it uses
(rN, fN, vN; CR/XER/CTR are locals in this codegen already) in C++ locals: loaded from ctx at entry, written back
before every call and every return, reloaded after every call. Exact by construction, and faster on the Switch: GCC
keeps the locals in registers instead of re-reading ctx after every guest store (ctx could alias guest memory under
-fno-strict-aliasing).

For each function two headers are written under app/generated/hot/<module>/:
  n_<ADDR>.h         Native(ctx, base): the copy; its calls go where the generated code sends them.
  n_<ADDR>_record.h  Record(ctx, base): the same body for the guard (futari_native_hot.cpp). It is included after
                     futari_native_hot_record.h, which makes every guest memory access log the bytes it touches, and
                     its calls go to the callees' Record() (so the guard can roll the whole call tree back). A function
                     whose calls are not all localized (indirect calls included) gets no record header: unguarded.
The __save*/__rest* helpers are called as they are (they store the saved registers into the stack frame: the same
bytes in both runs).

  python tools/localize_native.py --list app/src/native/hot_functions.txt [--gen app/generated]
  list lines: <module> <ADDR> [callee] <what>   ("callee" = localized only as a guard helper, no hook)
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+)\b')
CALL = re.compile(r'\b((?:__imp__)?(sub_[0-9A-F]{8})|__(?:save|rest)[a-z]+_\d+)\(ctx, base\);')
INDIRECT = re.compile(r'\bREX_CALL_INDIRECT_FUNC(?:_AT)?\(')
# A leaf function rewritten by tools/leaf_locals.py no longer says ctx.<register>: its declarations name the registers.
LEAF_REG = re.compile(r'\bREX_LEAF_LOCAL\(\w+, l_\w+, (\w+)\)')
# tools/indirect_calls.py: a promoted call names its target's hookable symbol in its PC self-check.
PROMOTED = re.compile(r'\bREX_PROMOTED_CALL\(0x[0-9A-F]{8}, (sub_[0-9A-F]{8})\)')


def find_body(folder, name):
    for path in sorted(glob.glob(os.path.join(folder, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8', errors='replace').read()
        m = re.search(r'^DEFINE_REX_FUNC\(' + name + r'\) \{\n(.*?)^\}\n', text, re.S | re.M)
        if m:
            return unleaf(m.group(1)), path
    return None, None


def unleaf(body):
    """The function's text as it was before tools/leaf_locals.py (which runs after this tool in a fresh tree, but
    before it when tools/post_codegen.py is run again on a tree it already processed): ctx.<register> again, without
    the declarations and the stores before the returns. This tool then makes its own locals of them. (sa2-nx's.)"""
    regs = LEAF_REG.findall(body)
    if not regs:
        return body
    names = re.compile(r'\bl_(%s)\b' % '|'.join(re.escape(r) for r in regs))
    out = []
    for l in body.split('\n'):
        if re.match(r'\tREX_LEAF_LOCAL\(', l) or re.match(r'\t(?:REX_LEAF_STORE\(l_\w+, \w+\);\s?)+$', l):
            continue
        l = re.sub(r'\{ (?:REX_LEAF_STORE\(l_\w+, \w+\); )+return; \}', 'return;', l)
        out.append(names.sub(lambda m: 'ctx.' + m.group(1), l))
    if 'REX_LEAF_' in '\n'.join(out):
        sys.exit('localize_native: a leaf-local form this tool does not know (tools/leaf_locals.py changed?)')
    return '\n'.join(out)


def type_of(reg):
    return 'PPCVRegister' if reg.startswith('v') else 'PPCRegister'


def emit(addr, module, what, body, rel, record, localized):
    lines = body.splitlines()
    regs = sorted({m.group(1) for l in lines for m in REG.finditer(l)},
                  key=lambda r: (r[0], int(r[1:])))
    out_regs = '; '.join(f'ctx.{r} = l_{r}' for r in regs) or '(void)0'
    in_regs = '; '.join(f'l_{r} = ctx.{r}' for r in regs) or '(void)0'

    def call(m):
        whole, imp_name, sub = m.group(0), m.group(1), m.group(2)
        target = whole
        if record and sub:
            target = f'::futari::hot::n_{sub[4:]}::Record(ctx, base);'
        return '{ FUTARI_SYNC_OUT(); ' + target + ' FUTARI_SYNC_IN(); }'

    code, calls = [], 0
    for l in lines:
        if l.strip().startswith('//'):
            continue  # the PPC disassembly comments
        l = REG.sub(lambda m: 'l_' + m.group(1), l)
        l, n = CALL.subn(call, l)
        calls += n
        # Returns get braces: the generated code writes them as the body of one-line ifs too.
        l = re.sub(r'\breturn;', '{ FUTARI_SYNC_OUT(); return; }', l)
        code.append(l)
    code.append('\tFUTARI_SYNC_OUT();')
    decl = '\n'.join(f'  {type_of(r)} l_{r} = ctx.{r};' for r in regs)
    externs = ''.join(f'REX_EXTERN({c});\n' for c in sorted({m.group(1) for m in CALL.finditer(body)} |
                                                            {m.group(1) for m in PROMOTED.finditer(body)}))
    fn = 'Record' if record else 'Native'
    head = 'inline void Native' if not record else 'void Record'
    return f'''// sub_{addr} ({module}): {what}. {fn}() for futari_native_hot.cpp.
// GENERATED by tools/localize_native.py from {rel} (do not edit; re-run the tool after a codegen change): the
// recompiled function's own statements, {len(regs)} guest registers in C++ locals, written back before each of its
// {calls} call(s) and at every return.
#pragma once

{externs}
namespace futari::hot::n_{addr} {{

{head}(PPCContext& ctx, uint8_t* base) {{
{decl}
#define FUTARI_SYNC_OUT() do {{ {out_regs}; }} while (0)
#define FUTARI_SYNC_IN() do {{ {in_regs}; }} while (0)
''' + '\n'.join(code) + f'''
#undef FUTARI_SYNC_OUT
#undef FUTARI_SYNC_IN
}}

}}  // namespace futari::hot::n_{addr}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', required=True)
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    a = ap.parse_args()
    entries = []
    for line in open(a.list, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 3)
        module, addr = parts[0], parts[1].upper()
        callee = len(parts) > 2 and parts[2] == 'callee'
        what = ' '.join(parts[3 if callee else 2:])
        entries.append((module, addr, callee, what))
    localized = {(m, ad) for m, ad, _, _ in entries}
    for module, addr, callee, what in entries:
        body, path = find_body(os.path.join(a.gen, module), 'sub_' + addr)
        if body is None:
            sys.exit(f'{module}: sub_{addr} not found')
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        out_dir = os.path.join(a.gen, 'hot', module)
        os.makedirs(out_dir, exist_ok=True)
        callees = {m.group(2) for m in CALL.finditer(body) if m.group(2)}
        missing = sorted(c for c in callees if (module, c[4:]) not in localized)
        guarded = not INDIRECT.search(body) and not missing
        open(os.path.join(out_dir, f'n_{addr}.h'), 'w', encoding='utf-8', newline='\n').write(
            emit(addr, module, what, body, rel, False, localized))
        record_path = os.path.join(out_dir, f'n_{addr}_record.h')
        if guarded:
            open(record_path, 'w', encoding='utf-8', newline='\n').write(
                emit(addr, module, what, body, rel, True, localized))
        elif os.path.exists(record_path):
            os.remove(record_path)
        n_regs = len({m.group(1) for m in REG.finditer(body)})
        print(f'{module} sub_{addr}: {n_regs} registers localized, '
              + ('guarded' if guarded else 'UNGUARDED (' + ('indirect call' if INDIRECT.search(body) else
                                                            'calls ' + ', '.join(missing)) + ')')
              + (' [callee]' if callee else ''))


if __name__ == '__main__':
    main()
