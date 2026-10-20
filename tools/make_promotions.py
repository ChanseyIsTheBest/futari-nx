#!/usr/bin/env python3
"""Makes app/indirect_promotions/<module>.txt for default.xex and the eight game cores from census files of the PC
build (tools/indirect_promotions.py once per module; tools/post_codegen.py applies the lists at the next codegen).

A census file is what a PC run writes with futari_diag_indirect_census = <file>: per module and call site, how often
it ran and where it went. Each game core only runs in its own modes, so the census needs a session of every one:
the top menu's four entries with both panels load the eight cores (2026-10-14: reco, palm, kiniro and hirow, normal
and abnormal, in that order of the menu).

  python tools/make_promotions.py out/pc/census/*.txt
The lists are inputs of the generated code: change them only before an instrumented (PGO) build, never between it
and the build that uses its profile.
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREES = ('default', 'palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal')


def main():
    files = [f for pattern in sys.argv[1:] for f in sorted(glob.glob(pattern))]
    if not files:
        sys.exit(__doc__)
    out = os.path.join(ROOT, 'app', 'indirect_promotions')
    os.makedirs(out, exist_ok=True)
    for tree in TREES:
        gen = os.path.join(ROOT, 'app', 'generated', tree)
        result = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'indirect_promotions.py'), '--census', *files,
                                 '--module', tree, '--gen', gen, '--out', os.path.join(out, tree + '.txt')],
                                capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(result.stdout + result.stderr)
        print('%-16s %s' % (tree, result.stdout.splitlines()[0][len('indirect_promotions: '):] if result.stdout else ''))


if __name__ == '__main__':
    main()
