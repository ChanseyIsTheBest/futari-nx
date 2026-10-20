#!/usr/bin/env python3
"""Whether guest code reads what memcpy and memset leave in r0 / r4-r12 (futari-nx, 2026-10-19; the assumption behind
tools/inline_memcalls.py and the natives of app/src/native/futari_native_crt.h).

The XDK's memcpy and memset leave r0 and r4-r12 changed; the natives (on since 1.0.0) and the inline forms of
tools/inline_memcalls.py leave them as they were. That is exact while nothing reads one of those registers after the
call before writing it. Compiled code cannot: for the compiler both routines are external assembly that clobbers every
volatile register (the calling convention). What could is hand-written code, or a value read and then not used.

This runs the whole-program analysis of tools/dead_ctx_defs.py on a generated module folder and follows, from every
call site of the two routines, what GUEST INSTRUCTIONS read before writing: in the calling function, in the functions
it calls directly (their reads before writing, to a fixed point); what runs after the caller returns is not followed
(counted). A call the analysis cannot see into (indirect, hooked, an import) counts as reading no scratch register
here: it can only receive one as an argument the caller wrote, by the rule above.

Result of 2026-10-19 (default.xex): 161 of 830 sites listed, and the ones looked at are reads that do not matter: a
callee that spills its argument registers r3-r10 to the stack in its prologue (varargs style) whatever the caller set,
and `mr r8,r8` (a no-op hint). So this is NOT a proof: the assumption rests on the calling convention, as the shipped
natives' always has; the list is where to look if a difference ever points at a memcpy or memset caller.

  python tools/memcalls_liveout.py <generated module folder> [--image out/<module>_image.bin]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dead_ctx_defs as dcd  # noqa: E402
import find_memcalls  # noqa: E402
import leaf_dead_stores as lds  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gen')
    ap.add_argument('--image')
    a = ap.parse_args()
    tree = lds.futari_modules.tree_of(a.gen)
    image = a.image or lds.futari_modules.image_path(tree)
    memcpy, memset, _ = find_memcalls.find(a.gen)
    targets = {int(memcpy[0][1][4:], 16): 'memcpy', int(memset[0][1][4:], 16): 'memset'}
    bodies, parsed, _, _ = dcd.analysis(a.gen, image, abi_indirect=True)
    scratch = (lds.R(0) | sum(lds.R(n) for n in range(4, 13))) & ~lds.R(3)

    # The reads of guest instructions only: what the analysis assumes of a call it cannot see into is left out.
    def own_use(node):
        if node.use in (lds.ALL, dcd.ABI_IN) and (node.call is None or not node.call[1]):
            return node.use & ~scratch
        return node.use

    ue = {}
    summary = {}
    for addr, nodes in parsed.items():
        undefined = lds.forward_undefined(nodes)
        local, calls = 0, []
        for node, state in zip(nodes, undefined):
            local |= own_use(node) & state
            if node.call and node.call[1]:
                calls.append((node.call[0], state))
        summary[addr] = (local, calls)
        ue[addr] = local
    callers = {}
    for addr, (_, calls) in summary.items():
        for callee, _ in calls:
            callers.setdefault(callee, set()).add(addr)
    work = set(summary)
    while work:
        addr = work.pop()
        local, calls = summary[addr]
        new = local
        for callee, state in calls:
            new |= ue.get(callee, 0) & state
        if new != ue[addr]:
            ue[addr] = new
            work |= callers.get(addr, set())

    def use_of(node):
        use = own_use(node)
        if node.call and node.call[1] and node.call[0] not in targets:
            use |= ue.get(node.call[0], 0)
        return use

    sites = bad = exits = 0
    for addr, nodes in parsed.items():
        live = lds.backward(nodes, use_of, 0)
        for i, node in enumerate(nodes):
            if not node.call or node.call[0] not in targets:
                continue
            sites += 1
            after = 0
            for s in node.succ:
                after |= live[s]
            if node.exit:
                exits += 1
            read = after & scratch
            if read:
                bad += 1
                if bad <= 40:
                    print('sub_%08X calls %s at instruction %d; guest instructions may read after it: %s' % (
                        addr, targets[node.call[0]], i, ' '.join(lds.reg_name(b) for b in range(64) if read >> b & 1)))
    print('memcalls_liveout %s: %d call sites of %s: %d where guest instructions read r0 / r4-r12 before writing them '
          '(%d sites can return to their caller\'s callers with the registers as the call left them: not followed)' % (
              tree, sites, ' and '.join('%s sub_%08X' % (n, k) for k, n in targets.items()), bad, exits))


if __name__ == '__main__':
    main()
