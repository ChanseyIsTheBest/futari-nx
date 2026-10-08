#!/usr/bin/env python3
"""Writes patches/: every change this port makes to third-party code, as diffs against the upstream sources it was
taken from (sa2-nx's patches/, made the same way).

For each upstream, a local clone at the pinned commit gets this repository's files laid over it (added or replaced;
nothing upstream is deleted: the SDK patch leaves the folders the fork does not carry, such as docs/ and tests/, as
they are), and `git diff --binary` of the result is the patch. Line endings are LF, as git stores the files.

  python tools/make_patches.py --upstream <folder with rexglue-sdk-v0.10.0, FFmpeg-0604b46 and XenosRecomp clones>

The clones are only read (a shared local clone is made in out/patchwork); the patch is then checked with
`git apply --check` on another clean clone.
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'out', 'patchwork')

UPSTREAMS = [
    # (patch name, upstream folder, commit, [(path in this repository, path in upstream)] or a function)
    ('rexglue-sdk-v0.10.0.patch', 'rexglue-sdk-v0.10.0', 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518', 'sdk'),
    ('ffmpeg-0604b46.patch', 'FFmpeg-0604b46', '0604b464c7cb4ebc94940cf1f324a3b26b87717c', 'ffmpeg'),
    ('xenosrecomp-990d03b.patch', 'XenosRecomp', '990d03b28a27b50277ee5d8d942e1c5f873869d1', 'xenos'),
]


def run(*args, cwd=None, capture=False):
    r = subprocess.run(args, cwd=cwd, check=True, capture_output=capture, text=capture)
    return r.stdout if capture else None


def tracked(prefix):
    """The files of this repository under prefix that git tracks (or would track: the ignore rules apply)."""
    out = run('git', 'ls-files', '--cached', '--others', '--exclude-standard', '--', prefix, cwd=ROOT, capture=True)
    return [p for p in out.splitlines() if p]


def remove_tree(path):
    def onerror(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if os.path.exists(path):
        shutil.rmtree(path, onerror=onerror)


def overlay(kind):
    """[(source in this repository, destination in the upstream tree)]."""
    if kind == 'sdk':
        pairs = []
        for p in tracked('sdk'):
            rel = p[len('sdk/'):]
            if rel.startswith('thirdparty/') and rel not in ('thirdparty/CMakeLists.txt',
                                                              'thirdparty/ffmpeg-overlay/codec_list.c'):
                continue  # FFmpeg's own files are the FFmpeg patch; the rest of thirdparty is fetched unmodified
            pairs.append((p, rel))
        return pairs
    if kind == 'ffmpeg':
        return [(p, p[len('sdk/thirdparty/FFmpeg/'):]) for p in tracked('sdk/thirdparty/FFmpeg')]
    if kind == 'xenos':
        names = ['shader_recompiler.cpp', 'shader_recompiler.h', 'shader_common.h', 'pch_min.h']
        return [(f'shaders/XenosRecomp/{n}', f'XenosRecomp/{n}') for n in names
                if os.path.exists(os.path.join(ROOT, 'shaders', 'XenosRecomp', n))]
    raise ValueError(kind)


def clean_clone(source, commit, dest):
    remove_tree(dest)
    run('git', '-c', 'core.autocrlf=false', 'clone', '-q', '--shared', '--no-checkout', source, dest)
    run('git', '-c', 'core.autocrlf=false', 'checkout', '-q', commit, cwd=dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--upstream', required=True)
    a = ap.parse_args()
    os.makedirs(os.path.join(ROOT, 'patches'), exist_ok=True)
    for name, folder, commit, kind in UPSTREAMS:
        source = os.path.join(a.upstream, folder)
        work = os.path.join(WORK, folder)
        clean_clone(source, commit, work)
        pairs = overlay(kind)
        for src, dst in pairs:
            data = open(os.path.join(ROOT, src), 'rb').read()
            if b'\0' not in data[:8000]:
                data = data.replace(b'\r\n', b'\n')  # LF, as git stores the files
            target = os.path.join(work, dst)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, 'wb') as f:
                f.write(data)
        run('git', '-c', 'core.autocrlf=false', 'add', '-A', cwd=work)
        diff = subprocess.run(['git', '-c', 'core.autocrlf=false', 'diff', '--cached', '--binary', '--full-index'],
                              cwd=work, check=True, capture_output=True).stdout  # bytes: no newline translation
        stat_line = run('git', '-c', 'core.autocrlf=false', 'diff', '--cached', '--shortstat', cwd=work,
                        capture=True).strip()
        out = os.path.join(ROOT, 'patches', name)
        with open(out, 'wb') as f:
            f.write(diff)
        # check: the patch applies to another clean clone
        check = os.path.join(WORK, folder + '_check')
        clean_clone(source, commit, check)
        run('git', '-c', 'core.autocrlf=false', 'apply', '--check', out, cwd=check)
        remove_tree(check)
        print(f'{name}: {len(pairs)} files laid over {folder} @ {commit[:7]}; {stat_line}; applies cleanly')


if __name__ == '__main__':
    sys.exit(main())
