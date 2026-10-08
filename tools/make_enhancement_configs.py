#!/usr/bin/env python3
"""Writes the premade Switch configs from the enhancement table (app/src/futari_enhancements.cpp, kTable), so a new
enhancement only has to be added in one place:

  app/futari_switch_defaults.toml the release's FIXED settings, compiled into the NRO (app/CMakeLists.txt,
                                  src/futari_app.h BuiltInConfig) and applied before futari.toml: every enhancement at
                                  its current value (the experimental ones included), logging and profiling off
  app/futari_switch.toml          (hand-written, not generated) the player's futari.toml: the input options only;
                                  copied to the SD folder, and written next to the NRO by the game when missing
  app/configs/futari_all_on.toml  EVERYTHING on: every enhancement, the experimental (kExplicitOnly) ones included,
                                  profiler on
  app/configs/futari_cpu_on.toml  every CPU enhancement on (the experimental CPU ones included) and every non-CPU one,
                                  profiler on
  app/configs/futari_cpu_off.toml every CPU enhancement off (CPU master off and each key at its OFF value), non-CPU on,
                                  profiler on
  app/configs/futari_cpu_verify.toml  all on + futari_cpu_verify: every fast path's self-check compares on every use;
                                  the "[verify] ... differences: N" log lines must say 0. Slower: test sessions only
  app/configs/futari_test_direct_state_off.toml  all on except futari_cpu_direct_state (its console A/B)
  app/configs/futari_test_ab_<what>.toml         in-run A/B of one enhancement (on/off every 2 s, split per phase)
  app/configs/futari_test_log_writer_off.toml    all on with the old synchronous log (log_file_writer = false)
  app/configs/futari_profile.toml all on + the per-category GPU report (futari_gpu_marks_categories) and the memory
                                  log; with logs/rex/stacks_profile.flag the CPU sampler records every pc
                                  (tools/switch_hot_functions.py ranks the functions)

The configs differ only in what their name says, so two logs compare directly. Copy one to
sdmc:/switch/futari-nx/futari.toml (the game reads only that name).

  python tools/make_enhancement_configs.py [--sd out/switch_sd/switch/futari-nx]
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = """# Mushihimesama Futari (Switch) - {title}
# Written by tools/make_enhancement_configs.py from app/src/futari_enhancements.cpp: edit the table there, not this file.
# To use it: copy it to sdmc:/switch/futari-nx/ as futari.toml (the game reads only that name). The startup log line
# "[enhancements]" says which enhancements were active.
{extra}
log_level = "info"
log_to_file = true  # the release writes no log (its built-in settings): test configs turn it back on

# The native Vulkan renderer (NVK).
futari_renderer_native = true

# Safety net: shaders the package lacks are saved to shader_misses/ (send that folder back).
futari_shader_misses = "shader_misses"
"""


def parse_table(path):
    text = open(path, encoding='utf-8').read()
    rows = re.findall(r'\{"([a-z0-9_]+)",\s*(kCpu|kGpu),\s*"((?:[^"\\]|\\.)*)",\s*"([^"]*)",\s*"([^"]*)"'
                      r'(,\s*kExplicitOnly)?\}', text)
    if not rows:
        raise SystemExit('no rows found in kTable of ' + path)
    # kExplicitOnly rows are experimental: the configs write them OFF (set them to the ON value by hand to try them).
    # 'try' keeps the table's ON value (futari_all_on.toml sets the experimental rows to it).
    return [dict(key=k, cpu=g == 'kCpu', what=w, on=off if x else on, off=off, explicit=bool(x), try_=on)
            for k, g, w, on, off, x in rows]


def value(v):
    return v if v in ('true', 'false') or re.fullmatch(r'-?\d+', v) else '"%s"' % v


def section(rows, cpu, state, overrides):
    lines = []
    for r in rows:
        if r['cpu'] != cpu:
            continue
        v = overrides.get(r['key'], r['on'] if state else r['off'])
        note = '  (set by this config)' if r['key'] in overrides else ''
        if r.get('explicit') and r['key'] not in overrides:
            note += '  (EXPERIMENTAL, off by default; true to try)'
        lines.append(f"{r['key']} = {value(v)}".ljust(58) + f"# {r['what']}{note}")
    return '\n'.join(lines)


# The profiler: ON in every test config of configs/ (the user's request, 2026-10-06), OFF in the verify config and in
# the release (its built-in settings and the code defaults; the user's request, 2026-10-08). Plain cvars outside the
# enhancement table, so no master switch can turn profiling on or off.
PROFILER_ON = """
switch_profile_report = true              # the profile file (off in the release's built-in settings)
# ---- Profiler (on in every config but the verify one; outside the enhancement masters) ----
switch_profile_stacks = true              # CPU sampler: pc and stack of every busy thread at 1 kHz (rex_profile.log)
futari_gpu_timestamps = true              # GPU time per submission ("GPU per Swap" lines)
futari_gpu_marks_categories = true    # GPU time per pass category and the idle gap ("GPU per Swap" lines)
futari_memory_log_s = 1                   # one "[mem]" line per second
"""
PROFILER_OFF = """
# ---- Profiler OFF (verify runs measure correctness, not speed) ----
switch_profile_stacks = false
futari_gpu_timestamps = false
futari_gpu_marks_categories = false
futari_memory_log_s = 0
"""


def write(path, title, extra, rows, cpu_on, tail='', overrides=None, profiler=True):
    overrides = overrides or {}
    n_cpu = sum(r['cpu'] for r in rows)
    n_gpu = len(rows) - n_cpu
    text = HEADER.format(title=title, extra=extra)
    text += f"""
# ---- Enhancement masters (false turns every enhancement of that group off, whatever its own key says) ----
futari_cpu_enhancements = {'true' if cpu_on else 'false'}
futari_noncpu_enhancements = true

# ---- CPU enhancements ({n_cpu}) ----
{section(rows, True, cpu_on, overrides)}

# ---- Non-CPU enhancements ({n_gpu}) ----
{section(rows, False, True, overrides)}
"""
    text += PROFILER_ON if profiler else PROFILER_OFF
    text += tail
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sd', default=os.path.join(ROOT, 'out', 'switch_sd', 'switch', 'futari-nx'),
                    help='SD folder: futari.toml and configs/ are written there too')
    a = ap.parse_args()
    rows = parse_table(os.path.join(ROOT, 'app', 'src', 'futari_enhancements.cpp'))
    app = os.path.join(ROOT, 'app')
    profiling = ('# Profiler on (see the "Profiler" block): tools/switch_hot_functions.py logs/rex/rex_profile.log ranks\n'
                 '# the functions; the GPU time per pass category is in the "GPU per Swap" log lines.\n')
    deferred = dict(futari_native_deferred_recording='true', futari_deferred_native_fast='true',
                    futari_deferred_native_update='true')
    # Every experimental (kExplicitOnly) row at its ON value: futari_all_on.toml (the user's request, 2026-10-08).
    # Deferred recording included again (the user, 2026-10-08 evening): the console run without it slowed to ~40 fps
    # at ~1000 draws a frame (the ring cost 16-18 us per draw without it, 5-6 us with it, and the run with it was clean).
    experimental = {r['key']: r['try_'] for r in rows if r['explicit']}
    configs = {
        # name: (title, extra header text, CPU master, tail, overrides, profiler)
        # Everything on: every enhancement, the experimental ones included (deferred recording and the native Direct3D
        # layer), profiler on. Each experimental key says "(set by this config)": set one false to rule it out.
        'futari_all_on.toml': ('EVERYTHING ON: every enhancement, the experimental ones included (deferred recording, '
                               'the native Direct3D layer), profiler on',
                               profiling + '# The experimental keys (deferred recording, the native Direct3D layer '
                               'futari_api_*) are on here: if\n# something looks or runs wrong, set them false one at '
                               'a time.\n',
                               True, '', experimental, True),
        # Console test of the experimental deferred recording (its PC stalls were Windows sleep rounding, fixed
        # 2026-10-06; banjo-nx saw loading-screen hangs): cpu_on + the three deferred keys.
        'futari_cpu_on_deferred.toml': ('ALL CPU ENHANCEMENTS ON + EXPERIMENTAL deferred recording, profiler on',
                                        profiling, True, '', deferred, True),
        # The user's request (2026-10-08): cpu_on has every CPU key on, the experimental ones included (the same keys as
        # futari_all_on.toml: deferred recording and the native Direct3D layer).
        'futari_cpu_on.toml': ('ALL CPU ENHANCEMENTS ON (the experimental ones included: deferred recording, the native '
                               'Direct3D layer), profiler on', profiling,
                               True, '', {k: v for k, v in experimental.items()
                                          if next(r for r in rows if r['key'] == k)['cpu']}, True),
        'futari_cpu_off.toml': ('ALL CPU ENHANCEMENTS OFF (A/B baseline), profiler on', profiling, False, '', {}, True),
        'futari_cpu_verify.toml': (
            'CPU VERIFY: all on, every fast path checked against the original on every use; profiler off',
            '# Slower than futari_cpu_on.toml (every comparison runs the original path too): for a test session.\n'
            '# Every 10 s the log has "[verify] checks done: ... | differences: N": N must stay 0. A difference also\n'
            '# logs a DIFFERENCE or MISMATCH line and turns that one fast path off for the rest of the session.\n',
            True, '\n# ---- Verify mode ----\nfutari_cpu_verify = true\n', {}, False),
        'futari_profile.toml': ('PROFILE: all on, profiler on (the release has it off)', profiling, True, '',
                                {}, True),
        # The direct renderer's console A/B (sa2-nx's sa2_test_direct_state_off.toml): cpu_on without step 1, so the
        # "[native] ring thread: ..." lines of the two runs give its saving on the console.
        'futari_test_direct_state_off.toml': ('TEST: all on except the direct renderer (futari_cpu_direct_state), '
                                              'profiler on', profiling, True, '',
                                              dict(futari_cpu_direct_state='false'), True),
        # The same A/B inside ONE run (banjo-nx's method; sequential runs are too noisy): the enhancement alternates
        # on/off every 2 s and the "[native] A/B of ..." line splits the ring's work per draw by phase.
        'futari_test_ab_direct_state.toml': ('TEST: all on, the direct renderer alternating on/off every 2 s '
                                             '(in-run A/B), profiler on', profiling, True,
                                             '\n# ---- In-run A/B ----\nfutari_ab_s = 2\n'
                                             'futari_ab_key = "futari_cpu_direct_state"\n', {}, True),
        'futari_test_ab_ring_all.toml': ('TEST: all on, every ring enhancement of 2026-10-07 alternating on/off '
                                         'every 2 s together (in-run A/B), profiler on', profiling, True,
                                         '\n# ---- In-run A/B ----\nfutari_ab_s = 2\n'
                                         'futari_ab_key = "futari_ring_all"\n', {}, True),
        # The SDK's log writer thread (log_file_writer, 2026-10-07: the stutter of build M's cpu-on run was the ring
        # thread writing its report to the SD) off: the old synchronous log, for the console comparison.
        'futari_test_log_writer_off.toml': ('TEST: all on, the log written the old synchronous way (log_file_writer '
                                            'off), profiler on', profiling, True,
                                            '\n# ---- Log ----\nlog_file_writer = false\n', {}, True),
        # The native Direct3D layer (src/me/futari_api.cpp, 2026-10-07): cpu_on with its three keys on, and the same in
        # verify mode (every native call compared with Direct3D's original run on a device copy; "[api] ... verify: N
        # compared, 0 differences").
        'futari_test_api.toml': ('TEST: all on + the native Direct3D layer (futari_api_*), profiler on', profiling, True,
                                 '', dict(futari_api_draws='true', futari_api_window_scissor='true',
                                          futari_api_ring_zero_ahead='true', futari_api_shader_load='true',
                                          futari_api_clear='true', futari_api_resolve='true'), True),
        'futari_test_api_verify.toml': (
            'TEST: the native Direct3D layer in verify mode (every native call checked against the original)',
            '# Slower than futari_test_api.toml: for a test session. Every 10 s the log has "[api] native records ... '
            'verify: N\n# compared, 0 differences" and "[verify] ... differences: 0".\n',
            True, '\n# ---- Verify mode ----\nfutari_cpu_verify = true\n',
            dict(futari_api_draws='true', futari_api_window_scissor='true', futari_api_ring_zero_ahead='true',
                 futari_api_shader_load='true', futari_api_clear='true', futari_api_resolve='true'), False),
        'futari_test_ab_edram_memo.toml': ('TEST: all on, the EDRAM draw memo alternating on/off every 2 s '
                                           '(in-run A/B), profiler on', profiling, True,
                                           '\n# ---- In-run A/B ----\nfutari_ab_s = 2\n'
                                           'futari_ab_key = "futari_ring_edram_memo"\n', {}, True),
    }
    # Old configs go first: a renamed or dropped config must not linger next to the new ones (sa2-nx's SD folder kept a
    # stale test config with outdated keys).
    for folder in (os.path.join(app, 'configs'), os.path.join(a.sd, 'configs')):
        for old in glob.glob(os.path.join(folder, '*.toml')):
            os.remove(old)
    written = []
    for name, (title, extra, cpu_on, tail, overrides, profiler) in configs.items():
        for folder in (os.path.join(app, 'configs'), os.path.join(a.sd, 'configs')):
            write(os.path.join(folder, name), title, extra, rows, cpu_on, tail, overrides, profiler)
        written.append(name)
    # The release's fixed settings (the user, 2026-10-08: "hardcode everything current in the futari.toml", no logs):
    # every enhancement at its current value, the experimental ones included (as futari_all_on.toml), compiled in.
    write_builtin(os.path.join(app, 'futari_switch_defaults.toml'), rows, experimental)
    # The player's futari.toml (hand-written: the input options) goes to the SD folder as is.
    player = open(os.path.join(app, 'futari_switch.toml'), encoding='utf-8').read()
    with open(os.path.join(a.sd, 'futari.toml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(player)
    n_cpu = sum(r['cpu'] for r in rows)
    print(f'{len(rows)} enhancements ({n_cpu} CPU, {len(rows) - n_cpu} non-CPU); wrote {", ".join(written)}, '
          f'futari_switch_defaults.toml (built in) and {a.sd}/futari.toml (the player file)')


BUILTIN_HEADER = """# Mushihimesama Futari for the Switch: the fixed settings, compiled into the NRO (app/CMakeLists.txt,
# src/futari_app.h BuiltInConfig) and applied before the SD card's futari.toml, which holds the player's options
# (app/futari_switch.toml) and may still override any key here for testing (the configs/ folder does).
# Written by tools/make_enhancement_configs.py from app/src/futari_enhancements.cpp: edit the table there, not this file.
# Logging and profiling are off: no logs/ folder on the SD card.

# Game
futari_renderer_native = true
futari_shader_misses = ""

# No logging, no profiling
log_level = "off"
log_to_file = false
switch_profile_stacks = false
switch_profile_report = false
futari_gpu_timestamps = false
futari_gpu_marks_categories = false
futari_memory_log_s = 0
audio_pop_detect = false

# Enhancement masters (false turns every enhancement of that group off)
futari_cpu_enhancements = true
futari_noncpu_enhancements = true
"""


def write_builtin(path, rows, experimental):
    lines = [BUILTIN_HEADER]
    for cpu, title in ((True, 'CPU enhancements'), (False, 'Non-CPU enhancements')):
        lines.append(f'\n# {title}\n')
        for r in rows:
            if r['cpu'] != cpu:
                continue
            v = experimental.get(r['key'], r['on'])
            lines.append(f"{r['key']} = {value(v)}\n")
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(lines))


if __name__ == '__main__':
    main()
