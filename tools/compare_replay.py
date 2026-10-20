#!/usr/bin/env python3
"""Compares lockstep frame-hash recordings of a REPLAY (the game's own replay data played back): the test that the
game's logic did not change.

A replay is the one deterministic stretch of play Futari has: free play has random bullet patterns (two runs of one
build share no frame), and the menus and ranking screens have layers at independent phases. A replay's simulation is
fixed by its data, so every run draws the same game frames, but not all of them: the game's own frame skip follows
the wall clock, and where one run draws frames 1, 3, 5 another draws 2, 4, 6. Runs of the optimised PC build hardly
skip and share 99.9 % of their images; runs of an unoptimised build (which skips about every other frame) share
about 70 %, evenly over the whole replay. Only the labels the references cover are judged: a build that skips more
records further into the replay with the same number of frames.

A build whose game logic differs loses the replay's sync at the first difference: from there on its images are found
in no reference run at all. So the measure is, per stretch of the test recording, the share of its images that some
reference run shows within +-WINDOW labels, next to the same share for a control run (a reference run that is left
out of the set). A stretch far below the control is a divergence; shares like the control's are what an exact build
gives. The more reference runs, the higher both shares (three runs cover about 90 % of the frames).

  python tools/compare_replay.py TEST REF1 [REF2 ...] [--control RUN] [--window 2] [--stretch 300]
exit 1 when a stretch of TEST is found less than half as often as the lowest stretch of the control (or, without a
control, less than 25 %).
"""
import argparse
import sys


def load(path):
    frames = {}
    for line in open(path, encoding='utf-8'):
        parts = line.split()
        if len(parts) == 2 and parts[0].isdigit():
            frames[int(parts[0])] = parts[1]
    return frames


def shares(run, refs, window, stretch):
    """[(first label, last label, found, total)] per stretch of `run`'s frames, within the labels the references
    cover (a build that skips more frames records further into the replay with the same number of frames)."""
    where = {}
    for ref in refs:
        for label, h in ref.items():
            where.setdefault(h, []).append(label)
    first = max(min(ref) for ref in refs)
    last = min(max(ref) for ref in refs)
    labels = [label for label in sorted(run) if first <= label <= last]
    out = []
    for i in range(0, len(labels), stretch):
        part = labels[i:i + stretch]
        found = sum(1 for label in part if any(abs(x - label) <= window for x in where.get(run[label], ())))
        out.append((part[0], part[-1], found, len(part)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('test')
    ap.add_argument('refs', nargs='+')
    ap.add_argument('--control', help='a reference run that is not among the refs: what an exact build scores')
    ap.add_argument('--window', type=int, default=2)
    ap.add_argument('--stretch', type=int, default=300)
    a = ap.parse_args()
    test = load(a.test)
    refs = [load(p) for p in a.refs]
    if not test or not all(refs):
        sys.exit('compare_replay: an empty recording')
    rows = shares(test, refs, a.window, a.stretch)
    control = shares(load(a.control), refs, a.window, a.stretch) if a.control else None
    if not rows:
        sys.exit('compare_replay: the test recording has no frame in the labels the references cover')
    floor = min(f / t for _, _, f, t in control) / 2 if control else 0.25
    first, last = max(min(r) for r in refs), min(max(r) for r in refs)
    outside = sum(1 for label in test if not first <= label <= last)
    print('%d frames of %s against %d reference run(s), window +-%d; judged: labels %d..%d (%d frames outside)' %
          (len(test), a.test, len(refs), a.window, first, last, outside))
    bad = []
    for i, (first, last, found, total) in enumerate(rows):
        line = '  labels %6d..%-6d found %4d of %4d (%5.1f %%)' % (first, last, found, total, 100.0 * found / total)
        if control and i < len(control):
            _, _, cf, ct = control[i]
            line += '   control %4d of %4d (%5.1f %%)' % (cf, ct, 100.0 * cf / ct)
        if found / total < floor:
            line += '   <-- DIVERGED'
            bad.append(first)
        print(line)
    found = sum(r[2] for r in rows)
    total = sum(r[3] for r in rows)
    print('whole recording: %d of %d (%.1f %%)%s' % (
        found, total, 100.0 * found / total,
        '; control %.1f %%' % (100.0 * sum(r[2] for r in control) / sum(r[3] for r in control)) if control else ''))
    if bad:
        print('DIVERGED from label %d on (a stretch below %.0f %%)' % (bad[0], 100.0 * floor))
        sys.exit(1)
    print('no divergence (every stretch at or above %.0f %%)' % (100.0 * floor))


if __name__ == '__main__':
    main()
