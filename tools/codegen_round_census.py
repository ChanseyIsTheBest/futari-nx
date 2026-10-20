#!/usr/bin/env python3
"""Counts of the forms banjo-nx's codegen round writes into the generated code, per module (futari-nx, 2026-10-19;
docs/futari-notes.md, "Banjo's codegen round"). Read-only.

  python tools/codegen_round_census.py [--gen app/generated]
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TREES = ('default', 'palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal')
FORMS = [
    ('dcbt prefetch', r'\bREX_PREFETCH\('),
    ('rlwinm/rlwimi shifts', r'^\t// rl?wi(?:nm|mi)\b'),
    ('fp copy stores', r'= (?:\()*rex::ppc::QuietSingleBits\('),
    ('vperm1', r'\bREX_VPERM1\('),
    ('fp sign ops', r'\bREX_F(?:NEG|ABS|NABS)\('),
    ('leaf dead write-backs', r'\bREX_LEAF_DEAD_[RF]\('),
    ('single floats', r'\bREX_F(?:ADDS|SUBS|MULS|DIVS|SQRTS)\(|\bREX_FRSP_SINGLE\('),
    ('memcpy/memset inline', r'\bREX_MEM(?:CPY|SET)_CONST\('),
    ('stvlx/stvrx', r'\bREX_STV[LR]X\('),
    ('ro constants over the cfg', r'\bREX_RO_CFG_\w+\('),
    ('dead writes', r'\bREX_DEAD_(?:DEF|REC)\('),
    ('flush checks hoisted', r'\bREX_FLUSH_HOIST_KNOWN_(?:ON|OFF)\('),
    ('lr write-backs left out', r'\bREX_LR_WRITE_SAME\('),
    ('r1 mirror functions', r'\bREX_R1_LOCAL\(\)'),
    ('vsel', r'\bREX_VSEL\('),
    ('inline leaf calls', r'\bREX_INL\('),
]


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else os.path.join(ROOT, 'app', 'generated')
    table = collections.OrderedDict()
    for t in TREES:
        folder = os.path.join(gen, t)
        if not os.path.isdir(folder):
            continue
        text = ''.join(open(p, encoding='utf-8').read() for p in sorted(glob.glob(os.path.join(folder, '*_recomp.*.cpp'))))
        table[t] = [len(re.findall(rx, text, re.M)) for _, rx in FORMS]
    names = list(table)
    print('| Form | ' + ' | '.join(names) + ' |')
    print('|---|' + '---|' * len(names))
    for k, (label, _) in enumerate(FORMS):
        print('| %s | %s |' % (label, ' | '.join('{:,}'.format(table[n][k]) for n in names)))


if __name__ == '__main__':
    main()
