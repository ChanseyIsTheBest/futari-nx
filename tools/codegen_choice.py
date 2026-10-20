#!/usr/bin/env python3
"""The per-function choices of banjo-nx's codegen round, measured on the compiled A57 code (futari-nx, 2026-10-19;
docs/futari-notes.md, "Banjo's codegen round"). Three post-codegen steps pay in some functions and cost in others,
depending on register pressure in the code GCC makes; banjo-nx's rule (its docs/codegen-enhancements.md, "Loop and
leaf locals: choosing the functions"): measure every function both ways with the build's own compiler line, never
guess. Here:

  locals     tools/loop_locals.py / tools/leaf_locals.py: a function is OPTED OUT (app/locals_optout/<module>.txt)
             when its code without the locals (-DREX_LOOP_LOCALS_OFF -DREX_LEAF_LOCALS_OFF) is cheaper by more
             than --threshold percent;
  lr         tools/lr_preserve.py --rewrite: a function is CHOSEN (app/codegen_choice/<module>_lr_write_same.json)
             when its code with the proven write-backs left out is cheaper than with them (-DREX_LR_WRITE_SAME_OFF)
             by more than --margin percent;
  r1         tools/r1_local.py: CHOSEN (app/codegen_choice/<module>_r1_local.json) when the r1 mirror's code is
             cheaper than the original (-DREX_R1_LOCAL_OFF) by more than --margin percent (banjo-nx: the mirror in
             every function is a loss, +0.33 %; only the choice pays).

The measured tree is a COPY made by this script: the raw generator output (out/generated_raw, copied after a codegen
by the caller) processed by tools/post_codegen.py --gen <copy> --no-choice with no opt-out list, so every step applies
everywhere it can. Each generated file is compiled in four forms (A as built; B, C, D with one switch each) with the
Switch build's compile line of that module (tools/locals_choice.py: the build.ninja edge, without LTO), and each
function costed with locals_choice's loop-weighted count (an instruction in a loop weighs 10, a memory instruction
twice; trap-only paths left out). Static counts, not console samples: Futari's console profile is older than these
steps.

  python tools/codegen_choice.py --ninja out/switch/build.ninja [--work out/codegen_choice] [--modules a,b]
                                 [--jobs 3] [--threshold 2] [--margin 1] [--skip-process] [--write]
--write puts the lists into app/locals_optout and app/codegen_choice (otherwise they stay in --work); a new list needs
a fresh codegen (the tools never undo a function they converted): tools/codegen.ps1 -Force -Perf.
"""
import argparse
import concurrent.futures
import datetime
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import locals_choice as lc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
TREES = ('default', 'palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal')
FORMS = {'A': [], 'B': ['-DREX_LOOP_LOCALS_OFF', '-DREX_LEAF_LOCALS_OFF'], 'C': ['-DREX_LR_WRITE_SAME_OFF'],
         'D': ['-DREX_R1_LOCAL_OFF']}
COST = 6  # locals_choice.measure: the loop-weighted cost by the cfg method


def functions_with(path, marker):
    """Names of the functions of one generated file whose text has `marker`."""
    found, name, hit = set(), None, False
    for line in open(path, encoding='utf-8'):
        if name is None:
            m = lc.FUNC.match(line.rstrip('\n'))
            if m:
                name, hit = m.group(1), False
            continue
        if line.startswith('}'):
            if hit:
                found.add(name)
            name = None
            continue
        if marker in line and not line.lstrip().startswith('//'):
            hit = True
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ninja', required=True)
    ap.add_argument('--work', default=os.path.join(ROOT, 'out', 'codegen_choice'))
    ap.add_argument('--modules', default=','.join(TREES))
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--threshold', type=float, default=2.0)
    ap.add_argument('--margin', type=float, default=1.0)
    ap.add_argument('--skip-process', action='store_true', help='measure the copy as it is (already processed)')
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    gen_copy = os.path.join(work, 'generated')
    modules = [m for m in a.modules.split(',') if m]
    if not a.skip_process:
        raw = os.path.join(ROOT, 'out', 'generated_raw')
        if not os.path.isdir(raw):
            sys.exit('codegen_choice: no out/generated_raw (copy app/generated/<module> there right after a codegen)')
        shutil.rmtree(gen_copy, ignore_errors=True)
        for m in modules:
            shutil.copytree(os.path.join(raw, m), os.path.join(gen_copy, m))
        # No opt-out list for the measurement: every function the locals tools can take, takes them.
        env = dict(os.environ, FUTARI_LOCALS_OPTOUT_DIR=os.path.join(work, 'no_optout'))
        os.makedirs(env['FUTARI_LOCALS_OPTOUT_DIR'], exist_ok=True)
        p = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'post_codegen.py'), '--gen', gen_copy,
                            '--no-choice'], cwd=ROOT, env=env)
        if p.returncode:
            sys.exit('codegen_choice: post_codegen.py failed on the copy')
    summary = []
    for m in modules:
        gen = os.path.join(gen_copy, m)
        out = os.path.join(work, m)
        for f in FORMS:
            os.makedirs(os.path.join(out, 'obj' + f), exist_ok=True)
        cmd, prefix, orig_gen, _ = lc.compile_command(a.ninja, gen, out)
        files = sorted((int(lc.TU.search(f).group(2)), f) for f in os.listdir(gen)
                       if lc.TU.search(f) and lc.TU.search(f).group(1) == prefix)
        marked = {}
        for n, f in files:
            path = os.path.join(gen, f)
            marked[n] = {'B': lc.changed_functions(path), 'C': functions_with(path, 'REX_LR_WRITE_SAME('),
                         'D': functions_with(path, 'REX_R1_LOCAL()')}
        work_files = [n for n, _ in files if any(marked[n].values())]
        print('codegen_choice %s: %d of %d files to compile in %d forms; %s' % (
            m, len(work_files), len(files), len(FORMS), subprocess.list2cmdline(cmd[:3]) + ' ...'), flush=True)

        def build(n):
            src = os.path.join(gen, '%s_recomp.%d.cpp' % (prefix, n))
            costs = {}
            for form, extra in FORMS.items():
                if form != 'A' and not marked[n][form]:
                    continue
                obj = os.path.join(out, 'obj' + form, '%d.o' % n)
                if not (os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src)):
                    p = subprocess.run(cmd + extra + ['-o', obj, '-c', src], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
                    if p.returncode:
                        raise RuntimeError('compile %s (%s):\n%s' % (src, form, p.stdout[-4000:]))
                costs[form] = lc.function_costs(obj, 'cfg')
                os.remove(obj)
            return n, costs

        results = {}
        start = datetime.datetime.now()
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            for k, fut in enumerate(concurrent.futures.as_completed([ex.submit(build, n) for n in work_files])):
                n, costs = fut.result()
                results[n] = costs
                if (k + 1) % 5 == 0 or k + 1 == len(work_files):
                    print('  %s: %d / %d files (%s)' % (m, k + 1, len(work_files),
                                                       str(datetime.datetime.now() - start)[:7]), flush=True)
        optout, lr, r1 = [], [], []
        totals = {'B': [0, 0, 0], 'C': [0, 0, 0], 'D': [0, 0, 0]}  # [A total, form total, A with the choice]
        for n, costs in results.items():
            A = costs['A']
            for form in ('B', 'C', 'D'):
                names = marked[n][form] if form != 'B' else set(marked[n]['B'])
                X = costs.get(form, {})
                for name in names:
                    if name not in A or name not in X or not A[name][COST]:
                        continue
                    ca, cx = A[name][COST], X[name][COST]
                    totals[form][0] += ca
                    totals[form][1] += cx
                    if form == 'B':
                        loser = cx < ca * (1 - a.threshold / 100)  # off cheaper: opt out
                        totals[form][2] += cx if loser else ca
                        if loser:
                            for tool in marked[n]['B'][name][0].split('+'):
                                optout.append('%s %s' % (tool, name))
                    else:
                        winner = ca < cx * (1 - a.margin / 100)  # on cheaper than off: chosen
                        totals[form][2] += ca if winner else cx
                        (lr if form == 'C' else r1).append(name) if winner else None
        line = ('%s: locals: cost on %d, off %d, chosen %d (%d opted out) | lr write-backs: on %d, off %d, chosen %d '
                '(%d functions) | r1 mirror: on %d, off %d, chosen %d (%d functions)' % (
                    m, totals['B'][0], totals['B'][1], totals['B'][2], len(optout), totals['C'][0], totals['C'][1],
                    totals['C'][2], len(lr), totals['D'][0], totals['D'][1], totals['D'][2], len(r1)))
        print(line, flush=True)
        summary.append(line)
        stamp = datetime.date.today().isoformat()
        with open(os.path.join(out, 'locals_optout.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('# %s: guest functions whose A57 code is cheaper WITHOUT the loop / leaf locals (tools/codegen_choice.py,\n'
                    '# %s; the Switch build\'s compile line without LTO, loop-weighted cost, off cheaper by more than\n'
                    '# %.1f %%). "<tool> <name>": tool = loop (tools/loop_locals.py) or leaf (tools/leaf_locals.py).\n'
                    % (m, stamp, a.threshold))
            for e in sorted(set(optout)):
                f.write(e + '\n')
        json.dump(sorted(set(lr)), open(os.path.join(out, 'lr_write_same.json'), 'w'), indent=0)
        json.dump(sorted(set(r1)), open(os.path.join(out, 'r1_local.json'), 'w'), indent=0)
        if a.write:
            os.makedirs(os.path.join(APP, 'locals_optout'), exist_ok=True)
            os.makedirs(os.path.join(APP, 'codegen_choice'), exist_ok=True)
            shutil.copyfile(os.path.join(out, 'locals_optout.txt'), os.path.join(APP, 'locals_optout', m + '.txt'))
            shutil.copyfile(os.path.join(out, 'lr_write_same.json'),
                            os.path.join(APP, 'codegen_choice', m + '_lr_write_same.json'))
            shutil.copyfile(os.path.join(out, 'r1_local.json'), os.path.join(APP, 'codegen_choice', m + '_r1_local.json'))
        for f in FORMS:
            shutil.rmtree(os.path.join(out, 'obj' + f), ignore_errors=True)
    open(os.path.join(work, 'summary.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(summary) + '\n')
    print('codegen_choice: done; lists in %s%s' % (work, ' and in app/' if a.write else ''))


if __name__ == '__main__':
    main()
