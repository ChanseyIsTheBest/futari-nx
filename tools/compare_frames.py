#!/usr/bin/env python3
"""Compares lockstep frame-hash recordings (futari_video_hashes = true): pixel identity of renderer settings.

Each run writes captures/video_<s>s.frames, one line per recorded frame: '<game frame> <XXH3 of the full image>'. With
vblank_per_swap the game advances one frame per swap whatever the PC's speed, so runs show the same images at the same
game frames (sa2-nx's method). Two things blur the labels:
  - the recorder samples the presented image a few ms after each swap, and under load the ring presents a frame late:
    an image can carry its label +-1 (compare recordings made with futari_video_every = 1);
  - Futari is not fully deterministic (title animations on wall-clock time, some gameplay frames).
So a frame of the test run counts as identical when its hash is among the baseline's hashes within +-WINDOW labels,
and only labels the baselines reproduce (every baseline has the same image within +-WINDOW) are judged.

  python tools/compare_frames.py TEST BASE1 [BASE2 ...] [--window 2] [--any]

--any: a test frame is identical when its image is ANY baseline's within +-WINDOW labels (not only the first one's).
Two baselines of the same settings are often a frame or two apart on wall-clock animations; a test run can follow
either one. An image no baseline showed near that label still counts as a difference.
  python tools/compare_frames.py A B          (two runs: the old any-offset comparison of B against A)
"""
import argparse


def load(path):
    frames = {}
    for line in open(path, encoding='utf-8'):
        parts = line.split()
        if len(parts) == 2:
            frames[int(parts[0])] = parts[1]
    return frames


def near(frames, label, window):
    return {frames[k] for k in range(label - window, label + window + 1) if k in frames}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('test')
    ap.add_argument('bases', nargs='+')
    ap.add_argument('--window', type=int, default=2)
    ap.add_argument('--any', action='store_true', help="identical when it matches any baseline (see above)")
    a = ap.parse_args()
    test = load(a.test)
    bases = [load(b) for b in a.bases]
    w = a.window
    judged = same = 0
    differ = []
    unreproduced = 0
    for label, h in sorted(test.items()):
        if any(label not in b for b in bases):
            continue
        # Reproduced: the first baseline's image at this label is within +-w labels of every other baseline.
        ref = bases[0][label]
        if not all(ref in near(b, label, w) for b in bases[1:]):
            unreproduced += 1
            continue
        judged += 1
        if h in near(bases[0], label, w) or (a.any and any(h in near(b, label, w) for b in bases[1:])):
            same += 1
        else:
            differ.append(label)
    print(f'test {len(test)} frames, {len(bases)} baseline(s); labels the baselines reproduce (+-{w}): {judged} '
          f'(not reproduced: {unreproduced})')
    print(f'identical to the baseline within +-{w} labels: {same} of {judged}')
    if differ:
        print('DIFFERENCES at', ' '.join(map(str, differ[:40])))
    distinct = len(set(test.values()))
    print(f'distinct images in the test run: {distinct}')
    print('PIXEL-IDENTICAL' if judged and not differ else 'DIFFERENCES FOUND' if differ else 'NOTHING COMPARED')


if __name__ == '__main__':
    main()
