#!/usr/bin/env python3
"""The post-codegen steps of the generated game code, in one place, for every generated folder of Futari (default.xex
and the eight game cores). sa2-nx's tools/post_codegen.py with its order of steps (docs/futari-notes.md, "The CPU
rounds of 1.0.1"), and since 2026-10-19 banjo-nx's codegen round on top (docs/futari-notes.md, "Banjo's codegen
round"); Futari's own tools/perf_codegen_post.py keeps the direct calls and the no-volatile header.

Run after EVERY codegen, by tools/codegen.ps1 -Perf and by the build itself (app/CMakeLists.txt appends this script
to the generated code's command: the build re-runs the generator on its own whenever an input changes, and that would
otherwise drop the steps silently).

  0. The late rewrites of an earlier run come out first (switch_goto, r1_local, lr_preserve, flush_hoist,
     inline_leaves, vsel_neon: banjo-nx's order), so every analysis below reads the text it was written for. On a
     fresh codegen they find nothing.
  1. tools/perf_codegen_post.py --no-direct --no-barriers   guest memory without volatile (the pch)
  2. tools/indirect_calls.py     every `bctrl` tagged with its site (PC census), and the sites of
                                 app/indirect_promotions/<module>.txt as a guarded direct call. Before the barriers
                                 (a loop needs none when every way round it passes an indirect call, and a promoted
                                 call is no longer one) and before the direct calls (a promoted call is one).
  3. tools/loop_barriers.py      REQUIRED with 1: without barriers GCC turns guest polling loops into infinite loops.
                                 sa2-nx's refinement: no barrier where it has nothing to protect; banjo-nx's narrow
                                 form (the guest window only; FUTARI_GEN_NARROW_LOOP_BARRIER).
  4. tools/pch_cold_traps.py     the pch's trap helper (before every division) out of line and cold
  5. tools/perf_codegen_post.py --no-volatile               direct calls of functions without a hook (and back to the
                                 hook for a function hooked since the last run)
  6. sasrt-nx's steps of 2026-10-12, each a rename of calls into macros the generated pch defines:
       tools/guest_cas.py          compare-and-swap without the legacy builtin's trailing `dmb ish` on AArch64
       tools/flush_mode_flow.py    the flush-mode checks the control flow and the callees' summaries fix: nothing on
                                   the Switch, still tested in the PC build. After the direct calls (the proofs
                                   depend on which callees are called directly), with --redo. The entry modes
                                   (--image) for default.xex only: they assume a function whose address is nowhere in
                                   its image is only reached by direct calls, and the game cores are DLLs reached
                                   through export tables of relative addresses.
       tools/vec_byteswap.py       the 16-byte reversal of guest vector loads and stores without a table lookup
       tools/pch_neon_shuffles.py  constant word shuffles as one permutation
  7. tools/ro_constants.py       loads from the image's read-only data as constants (never an import record: --xex).
                                 Before float_loads.py (it folds the integer form of a float load).
  8. tools/float_loads.py        a single-precision load straight into a float register
  9. tools/frame_traffic.py      (banjo-nx's round) the prologue / epilogue statements marked only: its three elisions
                                 stay off (sa2-nx: not exact). The marks are what steps 15 and 16 read.
 10. tools/loop_locals.py        the ctx registers a call-free loop uses as locals for the length of the loop;
     tools/leaf_locals.py        the ctx registers of leaf functions with loops as locals. Not in the functions of
                                 app/locals_optout/<module>.txt (compiled cheaper without: tools/locals_choice.py).
 11. banjo-nx's codegen round (from sasrt-nx's of 2026-10-14 and its own), in banjo-nx's order:
       tools/vperm_one_source.py   a vperm of one vector as a one-register lookup
       tools/fp_sign_ops.py        fneg / fabs / fnabs as float operations (the game never selects round up / down:
                                   the "[census] FPSCR rounding mode" line)
       tools/leaf_dead_stores.py   a leaf's write-back of a register no caller can read is skipped
       tools/single_floats.py      single-precision arithmetic in float where its inputs are proven singles
       tools/inline_memcalls.py    memcpy / memset of a constant size up to 64 bytes inline (each module's own CRT)
       tools/vec_unaligned_stores.py  stvlx / stvrx as a few stores instead of byte loops
       tools/ro_constants_cfg.py   the read-only constants step 7 cannot see across a label
       tools/dead_ctx_defs.py      guest register writes nothing reads, left out (--abi-indirect)
       tools/flush_hoist.py        a loop header's flush-mode check in front of the loop
     The whole-program analysis of leaf_dead_stores / dead_ctx_defs runs per module (tools/futari_modules.py: the
     entry point, a game core's exports). They and inline_memcalls leave the functions of the localized copies alone
     (their guard compares the whole context with the original's).
 12. tools/localize_native.py    the localized copies of the hottest functions (app/generated/hot), made from the
                                 text as it is now. Not for a scratch tree.
 13. tools/lr_preserve.py --rewrite   (banjo-nx's round) epilogue write-backs proven to store the value ctx.lr holds
                                 compile to nothing on the Switch (the PC build checks each: REX_LR_WRITE_SAME_CHECK),
                                 in the functions of app/codegen_choice/<module>_lr_write_same.json when it exists
 14. tools/r1_preserve.py, tools/r1_local.py   guest r1 as a local mirror where callees are proven to return it
                                 unchanged (the PC build checks the assumed calls: REX_R1_CHECK), in the functions of
                                 app/codegen_choice/<module>_r1_local.json when it exists
 15. tools/vsel_neon.py          VMX vsel as one NEON bit select
 16. tools/inline_leaves.py      static inline copies of tiny leaf functions in the files that call them. Last.
Not taken from banjo-nx's round (docs/futari-notes.md): tools/limiter_lazy_log10.py (no XAudio in Futari),
tools/switch_goto.py (no hot jump-table interpreter), tools/hot_layout.py (FUTARI_SWITCH_HOT_ORDER places the
profile's functions already), tools/store_cards.py (Renut's renderer only).

Steps 6 (default.xex), 7, 11 and 13-14 need the generator's image dumps (out/<module>_image.bin, written by
tools/codegen.ps1 through REXGLUE_DUMP_IMAGE) and steps 7 and 11 the modules' XEX files (app/futari_manifest.toml
names them).

Every step has its compile-time switch back (app/CMakeLists.txt, FUTARI_SWITCH_* and FUTARI_GEN_*), is idempotent, and
so is the sequence: a second run on a processed tree changes no file (the profile of a PGO build fits only the exact
text it was made from).

  python tools/post_codegen.py [--gen <generated folder>] [--hooked-list FILE] [--no-choice]
--no-choice: steps 13 and 14 in every function they can prove (the measurement tree of tools/locals_choice.py, and a
PC check of every proof), not only the chosen ones.
FUTARI_POST_CODEGEN_JOBS (default 3): how many modules run their steps at the same time (each touches only its
own folder; the output of a module comes as one block).
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
TREES = ('default', 'palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal')


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen: {os.path.basename(args[0])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def per_module(trees, steps, jobs):
    """steps(tree, run_one) for every module, up to `jobs` modules at a time (each touches only its own folder). The
    output of a module's steps is printed as one block when they are done; the first failure stops the script."""
    import concurrent.futures
    import threading
    lock = threading.Lock()

    def one(t):
        lines = []

        def run_one(*args):
            p = subprocess.run([sys.executable, *args], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace')
            lines.append(p.stdout)
            if p.returncode != 0:
                raise RuntimeError(f'{t}: {os.path.basename(args[0])} failed ({p.returncode})')
        try:
            steps(t, run_one)
        finally:
            with lock:
                sys.stdout.write(''.join(lines))
                sys.stdout.flush()

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        futures = [ex.submit(one, t) for t in trees]
        for f in futures:
            try:
                f.result()
            except RuntimeError as e:
                sys.exit(f'post_codegen: {e}')


def xex_files():
    """tree -> the module's XEX file, from app/futari_manifest.toml (file_path / out_directory_path pairs)."""
    text = open(os.path.join(APP, 'futari_manifest.toml'), encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'^file_path\s*=\s*"([^"]+)"\s*\n(?:[^\n\[]*\n)*?out_directory_path\s*=\s*"generated/(\w+)"',
                         text, re.M):
        out[m.group(2)] = os.path.normpath(os.path.join(APP, m.group(1)))
    return out


def main():
    args = sys.argv[1:]
    gen = args[args.index('--gen') + 1] if '--gen' in args else GEN
    hooked = ['--hooked-list', args[args.index('--hooked-list') + 1]] if '--hooked-list' in args else []
    choice = '--no-choice' not in args
    scratch = os.path.normcase(os.path.abspath(gen)) != os.path.normcase(GEN)
    scratch_gen = ['--gen', gen] if scratch else []
    trees = [t for t in TREES if os.path.isdir(os.path.join(gen, t))]
    folders = [os.path.join(gen, t) for t in trees]
    pchs = [os.path.join(f, 'futari_pch.h') for f in folders]
    xex = xex_files()
    images = {t: os.path.join(ROOT, 'out', f'{t}_image.bin') for t in trees}
    for t in trees:
        if not os.path.isfile(images[t]):
            sys.exit(f'post_codegen: {images[t]} is missing (the image dump of the code generator): run '
                     'tools\\codegen.ps1 -Perf once, it writes it (REXGLUE_DUMP_IMAGE)')
        if t not in xex or not os.path.isfile(xex[t]):
            sys.exit(f'post_codegen: no XEX file for {t} (app/futari_manifest.toml names {xex.get(t)})')
        if os.path.getmtime(xex[t]) > os.path.getmtime(images[t]):
            sys.exit(f'post_codegen: {images[t]} is older than {xex[t]}: run tools\\codegen.ps1 -Perf (it writes the '
                     'dump again)')
    os.environ.setdefault('REXSDK_DIR', os.path.join(ROOT, 'sdk'))
    proofs_root = os.path.join(ROOT, 'out', 'codegen_proofs', 'scratch' if scratch else 'build')
    # 0. The late rewrites of an earlier run out first (banjo-nx's order).
    for t, folder in zip(trees, folders):
        run(tool('r1_local.py'), '--gen', folder, '--restore')
        run(tool('lr_preserve.py'), '--gen', folder, '--out', os.path.join(proofs_root, t), '--restore')
        run(tool('flush_hoist.py'), '--gen', folder, '--restore')
        run(tool('inline_leaves.py'), '--gen', folder, '--restore')
        run(tool('vsel_neon.py'), '--gen', folder, '--restore')
    # 1. Guest memory without volatile.
    run(tool('perf_codegen_post.py'), '--no-direct', '--no-barriers', *scratch_gen)
    # 2. The tagged and promoted indirect calls.
    for t, folder in zip(trees, folders):
        run(tool('indirect_calls.py'), '--gen', folder, '--promote',
            os.path.join(APP, 'indirect_promotions', f'{t}.txt'))
    # 3. Loop barriers. 4. The cold trap helper.
    run(tool('loop_barriers.py'), *folders)
    run(tool('pch_cold_traps.py'), *pchs)
    # 5. Direct calls.
    run(tool('perf_codegen_post.py'), '--no-volatile', *scratch_gen, *hooked)
    jobs = int(os.environ.get('FUTARI_POST_CODEGEN_JOBS', '3'))

    def steps_a(t, run_one):
        folder = os.path.join(gen, t)
        # 6. sasrt-nx's steps.
        run_one(tool('guest_cas.py'), '--gen', folder)
        run_one(tool('flush_mode_flow.py'), '--redo', '--gen', folder,
                *(['--image', images[t]] if t == 'default' else []))
        run_one(tool('vec_byteswap.py'), '--gen', folder)
        run_one(tool('pch_neon_shuffles.py'), os.path.join(folder, 'futari_pch.h'))
        # 7. Read-only constants. 8. Float loads. 9. The frame marks. 10. Loop and leaf locals.
        run_one(tool('ro_constants.py'), '--gen', folder, '--image', images[t], '--xex', xex[t])
        run_one(tool('float_loads.py'), '--gen', folder)
        run_one(tool('frame_traffic.py'), '--gen', folder)
        run_one(tool('loop_locals.py'), '--gen', folder)
        run_one(tool('leaf_locals.py'), '--gen', folder)
        # 11. banjo-nx's codegen round.
        run_one(tool('vperm_one_source.py'), '--gen', folder)
        run_one(tool('fp_sign_ops.py'), '--gen', folder)
        run_one(tool('leaf_dead_stores.py'), '--gen', folder, '--image', images[t])
        run_one(tool('single_floats.py'), '--gen', folder)
        run_one(tool('inline_memcalls.py'), '--gen', folder)
        run_one(tool('vec_unaligned_stores.py'), '--gen', folder)
        run_one(tool('ro_constants_cfg.py'), '--gen', folder, '--image', images[t], '--xex', xex[t])
        if os.environ.get('FUTARI_DEAD_DEFS_OFF'):  # the step switched off: an earlier run's rewrites back out
            run_one(tool('dead_ctx_defs.py'), '--gen', folder, '--restore-only')
        else:
            run_one(tool('dead_ctx_defs.py'), '--gen', folder, '--image', images[t], '--abi-indirect')
        run_one(tool('flush_hoist.py'), '--gen', folder)

    per_module(trees, steps_a, jobs)
    # 12. The localized copies, from the text as it is now.
    if not scratch:
        run(tool('localize_native.py'), '--list', os.path.join(APP, 'src', 'native', 'hot_functions.txt'))
    def steps_b(t, run_one):
        folder = os.path.join(gen, t)
        # 13. Link-register write-backs. 14. r1 as a local mirror. (Both read the marks of step 9.)
        proofs = os.path.join(proofs_root, t)
        lr_choice = os.path.join(APP, 'codegen_choice', f'{t}_lr_write_same.json')
        r1_choice = os.path.join(APP, 'codegen_choice', f'{t}_r1_local.json')
        # An EMPTY choice list leaves the step out in that module (the tools would read an empty list as "every
        # function"); no list applies it everywhere it is proven.
        def chosen(path):
            if not choice or not os.path.isfile(path):
                return None
            return json.load(open(path, encoding='utf-8'))
        lr_list, r1_list = chosen(lr_choice), chosen(r1_choice)
        if lr_list != []:
            run_one(tool('lr_preserve.py'), '--gen', folder, '--out', proofs, '--assume-indirect', '--rewrite',
                    *(['--only-file', lr_choice] if lr_list is not None else []))
        if r1_list != []:
            run_one(tool('r1_preserve.py'), '--gen', folder, '--out', os.path.join(proofs, 'strict'))
            run_one(tool('r1_preserve.py'), '--gen', folder, '--out', os.path.join(proofs, 'assumed'),
                    '--indirect-preserve')
            run_one(tool('r1_local.py'), '--gen', folder, '--proof-strict',
                    os.path.join(proofs, 'strict', 'r1_preserve.json'), '--proof-assumed',
                    os.path.join(proofs, 'assumed', 'r1_preserve.json'),
                    *(['--only-file', r1_choice] if r1_list is not None else []))
        # 15. vsel. 16. The inline copies of tiny leaves, last of all.
        run_one(tool('vsel_neon.py'), '--gen', folder)
        run_one(tool('inline_leaves.py'), '--gen', folder)

    per_module(trees, steps_b, jobs)
    kept = '' if scratch else keep_times(folders + [os.path.join(gen, 'hot', t) for t in trees])
    print(f'post_codegen: steps done on {len(trees)} generated folders' + (f' ({gen})' if scratch else kept))


def keep_times(folders):
    """Gives every file whose text is what the previous run of this script left its time of then (sasrt-nx's idea).
    The build re-runs the generator whenever one of its inputs changes (a comment in a toml, the generator binary):
    it writes its raw text over the processed files, these steps make the same text of it again, and without this
    every generated file would be recompiled for nothing: ten minutes on the PC, the whole LTO link on the Switch.
    The texts and times of the last run are in out/post_codegen_state.json. (It assumes what is true of this tree:
    generated files are only ever written by the generator and by these steps, so nothing was compiled from another
    text in between.)"""
    import hashlib
    import json
    state_path = os.path.join(ROOT, 'out', 'post_codegen_state.json')
    try:
        old = json.load(open(state_path, encoding='utf-8'))
    except (OSError, ValueError):
        old = {}
    new = {}
    restored = 0
    # banjo-nx's keep_unchanged_times (2026-10-16), found here again on 2026-10-19: a time given back must never be
    # older than an input of the build's codegen step (codegen.d: the XEX files and the tomls; the generator binaries),
    # or ninja finds the step's outputs older than its inputs and runs the generator and these steps on every build.
    manifest = os.path.join(APP, 'futari_manifest.toml')
    inputs = set()
    for folder in folders:
        dep = os.path.join(folder, 'codegen.d')
        if os.path.isfile(dep):
            for m in re.finditer(r'([A-Za-z]:/[^\s\\]+(?:\\ [^\s\\]+)*)', open(dep, encoding='utf-8').read()):
                path = m.group(1).replace('\\ ', ' ')
                if os.path.normcase(os.path.abspath(path)) != os.path.normcase(manifest) and \
                        not path.endswith(('codegen.build.stamp', ':')):
                    inputs.add(path)
    for exe in ('rexglue.exe', 'rexgluerd.exe', 'rexglued.exe'):
        inputs.add(os.path.join(ROOT, 'sdk', 'out', 'win-amd64', exe))
    floor = max([os.stat(p).st_mtime_ns for p in inputs if os.path.isfile(p)] or [0])
    for folder in folders:
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if not name.endswith(('.cpp', '.h')):
                continue
            path = os.path.join(folder, name)
            digest = hashlib.sha1(open(path, 'rb').read()).hexdigest()
            key = os.path.relpath(path, ROOT).replace('\\', '/')
            time = os.stat(path).st_mtime_ns
            if key in old and old[key][0] == digest and max(old[key][1], floor) < time:
                when = max(old[key][1], floor)
                os.utime(path, ns=(when, when))
                time = when
                restored += 1
            elif time < floor:
                # A file neither the generator nor these steps wrote this time (the generator leaves unchanged files
                # alone): as an output of the step it must not be older than the step's inputs either.
                os.utime(path, ns=(floor, floor))
                time = floor
            new[key] = [digest, time]
    # The generator writes its manifest again on every run (the same text): as an input of its own step it was then
    # newer than every output kept above. It gets the time of the oldest output (an input as old as an output is not
    # newer: ninja keeps the step clean).
    generated = [os.stat(os.path.join(ROOT, k)).st_mtime_ns for k in new if os.path.isfile(os.path.join(ROOT, k))]
    if generated and os.path.isfile(manifest) and os.stat(manifest).st_mtime_ns > min(generated):
        oldest = min(generated)
        os.utime(manifest, ns=(oldest, oldest))
    os.makedirs(os.path.dirname(state_path), exist_ok=True)
    with open(state_path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(new, f)
    return f'; {restored} of {len(new)} files have the text of the last run and keep its time'


if __name__ == '__main__':
    main()
