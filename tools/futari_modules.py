#!/usr/bin/env python3
"""Futari's nine generated modules for the post-codegen tools that analyse the whole guest program (banjo-nx's codegen
round of 2026-10-19: tools/leaf_dead_stores.py and the tools built on it). default.xex and the eight game cores are
separate programs (the cores are DLLs, all at 0x88000000, loaded one at a time), so every analysis runs per module,
and what reaches a module from outside its own code differs by kind:

  - default.xex: its entry point (the PE header's AddressOfEntryPoint);
  - a game core: its entry point (DllMain) and its exports. default.xex calls the cores only through the addresses
    XexGetProcedureAddress gives it (sdk/src/system/xex_module.cpp): the XEX's export table (security info
    export_table, entries relative to imagebaseaddr << 16) or the PE export directory (relative to the image base).
    Neither form is a function's absolute address in the image, so banjo-nx's scan of the image's data words cannot
    see them.

  python tools/futari_modules.py [module ...]     (prints what it finds)
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
TREES = ('default', 'palm_normal', 'palm_abnormal', 'reco_normal', 'reco_abnormal', 'hirow_normal', 'hirow_abnormal',
         'kiniro_normal', 'kiniro_abnormal')


def tree_of(gen):
    """The module name of a generated folder (its last path component; a scratch copy keeps the names)."""
    name = os.path.basename(os.path.normpath(gen))
    if name not in TREES:
        sys.exit('futari_modules: %s is not one of the generated module folders %s' % (gen, ', '.join(TREES)))
    return name


def image_path(tree):
    return os.path.join(ROOT, 'out', '%s_image.bin' % tree)


def xex_files():
    """tree -> the module's XEX file, from app/futari_manifest.toml (file_path / out_directory_path pairs)."""
    text = open(os.path.join(APP, 'futari_manifest.toml'), encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'^file_path\s*=\s*"([^"]+)"\s*\n(?:[^\n\[]*\n)*?out_directory_path\s*=\s*"generated/(\w+)"',
                         text, re.M):
        out[m.group(2)] = os.path.normpath(os.path.join(APP, m.group(1)))
    return out


def xex_path(tree):
    return xex_files().get(tree)


def _opt_headers(d):
    count = struct.unpack_from('>I', d, 20)[0]
    return {struct.unpack_from('>I', d, 24 + 8 * i)[0]: struct.unpack_from('>I', d, 28 + 8 * i)[0]
            for i in range(count)}


def pe_header(img):
    """(image base, entry point address, [(section name, rva, size)], export directory (rva, size))."""
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    if img[pe:pe + 4] != b'PE\0\0':
        sys.exit('futari_modules: no PE header in the image dump')
    count = struct.unpack_from('<H', img, pe + 6)[0]
    opt = pe + 24
    entry_rva = struct.unpack_from('<I', img, opt + 16)[0]
    base = struct.unpack_from('<I', img, opt + 28)[0]
    export_dir = struct.unpack_from('<II', img, opt + 96)
    at = opt + struct.unpack_from('<H', img, pe + 20)[0]
    sections = []
    for _ in range(count):
        size, va = struct.unpack_from('<II', img, at + 8)
        sections.append((img[at:at + 8].rstrip(b'\0').decode('latin1'), va, size))
        at += 40
    return base, (base + entry_rva) if entry_rva else None, sections, export_dir


def outside_entries(tree, starts):
    """{function address: reason} of the functions this module's own code does not call but its loader or another
    module does: the entry point, the XEX header's entry point, and the exports (both tables, read both ways round
    where the byte order is not documented). `starts` is the set of function addresses of the tree."""
    img = open(image_path(tree), 'rb').read()
    base, entry, sections, (exp_rva, exp_size) = pe_header(img)
    out = {}
    if entry in starts:
        out[entry] = 'entry point'
    xex = xex_path(tree)
    if xex and os.path.isfile(xex):
        d = open(xex, 'rb').read()
        if d[:4] == b'XEX2':
            headers = _opt_headers(d)
            ep = headers.get(0x00010100)          # XEX_HEADER_ENTRY_POINT
            if ep in starts:
                out[ep] = 'entry point'
            security = struct.unpack_from('>I', d, 16)[0]
            # xex2_security_info: header_size, image_size, rsa_signature[256], unk_108, image_flags, load_address,
            # section_digest[20], import_table_count, import_table_digest[20], xgd2_media_id[16], aes_key[16],
            # export_table (a guest address inside the image).
            export_table = struct.unpack_from('>I', d, security + 4 + 4 + 256 + 4 + 4 + 4 + 20 + 4 + 20 + 16 + 16)[0]
            if export_table and base <= export_table < base + len(img):
                at = export_table - base
                # xex2_export_table: magic[3], modulenumber[2], version[3], imagebaseaddr, count, base, ordOffset[]
                imagebase, n = struct.unpack_from('>II', img, at + 32)
                for k in range(min(n, 4096)):
                    a = (struct.unpack_from('>I', img, at + 44 + 4 * k)[0] + (imagebase << 16)) & 0xFFFFFFFF
                    if a in starts:
                        out[a] = 'export'
    if exp_rva and exp_size and exp_rva + 40 <= len(img):
        for order in ('<', '>'):
            # IMAGE_EXPORT_DIRECTORY: NumberOfFunctions at +20, AddressOfFunctions at +28.
            n_functions = struct.unpack_from(order + 'I', img, exp_rva + 20)[0]
            functions_rva = struct.unpack_from(order + 'I', img, exp_rva + 28)[0]
            if 0 < n_functions < 4096 and functions_rva + 4 * n_functions <= len(img):
                for k in range(n_functions):
                    rva = struct.unpack_from(order + 'I', img, functions_rva + 4 * k)[0]
                    if base + rva in starts:
                        out[base + rva] = 'export'
    # Belt and braces: every word of an .edata section, either byte order, absolute or relative to the base.
    for name, va, size in sections:
        if name != '.edata':
            continue
        for k in range(va, min(va + size, len(img)) - 3, 4):
            for order in ('<', '>'):
                w = struct.unpack_from(order + 'I', img, k)[0]
                for a in (w, (base + w) & 0xFFFFFFFF):
                    if a in starts:
                        out[a] = 'export'
    return out


def crt_natives(gen):
    """The addresses of the module's memcpy and memset (tools/find_memcalls.py). Their calls go through Futari's
    native replacements (app/src/native/futari_native_crt.h), which read r3, r4, r5 and the bytes the originals read
    and write the bytes the originals write: for the whole-program analysis such a call reads what a call of the
    original reads (banjo-nx's hand-written natives, app/src/cpu/hot/hand.txt there). The analysis assumes nothing
    about what a callee writes, so the natives writing fewer registers changes nothing."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import find_memcalls
    memcpy, memset, _ = find_memcalls.find(gen)
    out = set()
    if len(memcpy) == 1:
        out.add(int(memcpy[0][1][4:], 16))
    if len(memset) == 1:
        out.add(int(memset[0][1][4:], 16))
    return out


def hot_functions(tree):
    """The functions of tools/localize_native.py's list for this module (app/src/native/hot_functions.txt), callees
    included: their localized copies are made from the text before the steps that drop or skip context writes, and
    the copies' guard compares the whole context with the original's, so those steps leave these functions alone."""
    path = os.path.join(APP, 'src', 'native', 'hot_functions.txt')
    names = set()
    if os.path.isfile(path):
        for line in open(path, encoding='utf-8'):
            parts = line.split('#', 1)[0].split()
            if len(parts) >= 2 and parts[0] == tree and re.fullmatch(r'[0-9A-Fa-f]{8}', parts[1]):
                names.add('sub_' + parts[1].upper())
    return names


def main():
    import glob
    for tree in sys.argv[1:] or TREES:
        gen = os.path.join(GEN, tree)
        starts = set()
        for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
            starts |= {int(x, 16) for x in re.findall(r'^DEFINE_REX_FUNC\((?:__imp__)?sub_([0-9A-F]{8})\)',
                                                      open(path, encoding='utf-8').read(), re.M)}
        img = open(image_path(tree), 'rb').read()
        base, entry, sections, export_dir = pe_header(img)
        found = outside_entries(tree, starts)
        print('%s: base 0x%08X, entry %s, export directory %s, %d functions; reached from outside: %s' % (
            tree, base, ('0x%08X' % entry) if entry else '-', export_dir, len(starts),
            ', '.join('sub_%08X (%s)' % kv for kv in sorted(found.items())) or 'nothing'))
        print('   sections: ' + ', '.join('%s 0x%X+0x%X' % s for s in sections))


if __name__ == '__main__':
    main()
