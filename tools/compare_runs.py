#!/usr/bin/env python3
"""Compares two raw recordings of the same scripted run (futari_video_s captures, e.g. the emulated Xenos GPU and the
native renderer) frame by frame, to find where the native renderer does not look identical.

The two runs drift a few frames apart (loading and recorder timing), so each frame of B is matched to the most similar
frame of A near the running offset (mean absolute difference of 80x45 grey thumbnails). For every matched pair the
full-size difference is measured; the report lists the worst stretches and writes, for each, one PNG with
A | B | |A-B| x4 side by side.

  python tools/compare_runs.py A.rgba B.rgba [--out folder] [--window 90] [--worst 8] [--threshold 24]

Each .rgba needs its .txt (width height fps count). Needs numpy and Pillow.
"""
import argparse
import os

import numpy as np
from PIL import Image


def load(path):
    w, h, fps, n = open(path[:-5] + '.txt').read().split()
    w, h, n = int(w), int(h), int(n)
    frames = np.memmap(path, dtype=np.uint8, mode='r', shape=(n, h, w, 4))
    return frames, float(fps)


def thumbs(frames, tw=80, th=45):
    n, h, w, _ = frames.shape
    out = np.empty((n, th, tw), np.float32)
    ys, xs = h // th, w // tw
    for i in range(n):
        f = frames[i, :th * ys, :tw * xs, :3].astype(np.float32).mean(axis=2)
        out[i] = f.reshape(th, ys, tw, xs).mean(axis=(1, 3))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--out', default=None)
    ap.add_argument('--window', type=int, default=90, help='frames searched each side of the running offset')
    ap.add_argument('--worst', type=int, default=8)
    ap.add_argument('--threshold', type=int, default=24, help='per-channel difference counted as a changed pixel')
    ap.add_argument('--step', type=int, default=2, help='compare every Nth frame of B')
    a = ap.parse_args()
    out = a.out or os.path.join(os.path.dirname(a.b), 'compare')
    os.makedirs(out, exist_ok=True)

    fa, _ = load(a.a)
    fb, _ = load(a.b)
    print(f'A {fa.shape}, B {fb.shape}: thumbnails...')
    ta, tb = thumbs(fa), thumbs(fb)

    # Starting offset: the best match of B's frame 0 over all of A (the runs reach each screen seconds apart).
    offset = int(np.abs(ta - tb[0]).mean(axis=(1, 2)).argmin())
    print(f'B frame 0 ~ A frame {offset}')
    rows = []  # (b index, a index, thumb mad, full mad, changed fraction)
    for i in range(0, len(fb), a.step):
        lo = max(0, i + offset - a.window)
        hi = min(len(fa), i + offset + a.window + 1)
        if lo >= hi:
            continue
        mads = np.abs(ta[lo:hi] - tb[i]).mean(axis=(1, 2))
        j = lo + int(mads.argmin())
        offset = j - i
        diff = np.abs(fa[j, :, :, :3].astype(np.int16) - fb[i, :, :, :3].astype(np.int16))
        changed = float((diff.max(axis=2) > a.threshold).mean())
        rows.append((i, j, float(mads.min()), float(diff.mean()), changed))

    rows_arr = np.array([r[3] for r in rows])
    changed_arr = np.array([r[4] for r in rows])
    with open(os.path.join(out, 'report.txt'), 'w') as f:
        f.write(f'pairs {len(rows)}; mean abs diff: median {np.median(rows_arr):.2f}, p90 {np.percentile(rows_arr, 90):.2f}, '
                f'max {rows_arr.max():.2f}; changed pixels (> {a.threshold}): median {np.median(changed_arr) * 100:.2f} %, '
                f'p90 {np.percentile(changed_arr, 90) * 100:.2f} %, max {changed_arr.max() * 100:.2f} %\n')
        for r in rows:
            f.write(f'B {r[0]:5d} A {r[1]:5d} thumb {r[2]:6.2f} mad {r[3]:6.2f} changed {r[4] * 100:6.2f} %\n')
    print(open(os.path.join(out, 'report.txt')).readline().strip())

    # The worst pairs, at least 60 frames apart from each other.
    chosen = []
    for k in np.argsort(-changed_arr):
        if all(abs(rows[k][0] - rows[c][0]) >= 60 for c in chosen):
            chosen.append(int(k))
        if len(chosen) >= a.worst:
            break
    for rank, k in enumerate(chosen):
        i, j, _, mad, changed = rows[k]
        A = fa[j, :, :, :3]
        B = fb[i, :, :, :3]
        D = np.clip(np.abs(A.astype(np.int16) - B.astype(np.int16)) * 4, 0, 255).astype(np.uint8)
        Image.fromarray(np.concatenate([A, B, D], axis=1)).save(
            os.path.join(out, f'worst_{rank:02d}_B{i:05d}_A{j:05d}.png'))
        print(f'worst {rank}: B frame {i} ~ A frame {j}: mad {mad:.2f}, changed {changed * 100:.2f} %')


if __name__ == '__main__':
    main()
