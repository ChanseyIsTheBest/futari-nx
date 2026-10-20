#!/usr/bin/env python3
"""Dump Mushihimesama Futari's Xbox 360 shader containers for Renut's shader cache.

Output: <out>/{ps,vs}_<xxh3_64 of container>.bin, each a complete Xbox 360 shader container (virtual header +
physical block, the layout XenosRecomp consumes), plus <out>/manifest.csv describing where each came from: the same
format reNut's tools/dump_shaders.py writes (banjo-nx), so the folder can be RENUT_SHADER_DUMP_DIR as it is.

Futari compiles its shaders into its executables, stored whole (constant tables included), in .rdata: default.xex
22 vertex + 22 pixel shader containers, every game core (media/Module/*, DLLs at 0x88000000) 2 + 2. The input is the
nine loaded PE images (out/<module>_image.bin, laid out by virtual address; image base from the PE header), so no XEX
decryption here. Containers (big-endian, XDK 2.0.9328):

  +0x00 flags 0x102A11xx (low bit 1 = vertex shader)     +0x10 constant table offset
  +0x04 virtual size (header part)                       +0x14 definition table offset (0: none)
  +0x08 physical size (literal constants + microcode)    +0x18 shader offset: u32 physical offset, u32 size, ...
  +0x0C 0x24 in default.xex (an .updb path block), 0 in the cores
  +0x1C, +0x20 zero (XenosRecomp only takes containers with these zero)

The physical block follows the virtual part directly, so a container is image[h : h + virtual + physical].
Candidates are validated as reNut's tool does (find_shader_headers / extract_from_xex), plus XenosRecomp's own
acceptance test and no overlap with the next container; every rejected candidate is listed with its reason.

The key is XXH3_64 (seed 0) of those virtual + physical bytes, which is what Renut computes at run time
(HashShaderContainer, gpu/render/shader_library.cpp) for D3DDevice_CreateVertexShader / CreatePixelShader and
XGRegister*Shader, and what XenosRecomp computes when it builds the cache, and the name format of Renut's own
captures (CaptureMissingShader: "{ps|vs}_{:016x}.bin").

The dump is game data: never commit it (out/ is ignored).

  python tools/renut_dump_shaders.py [--images DIR] [--out DIR] [--ucode] [module ...]
"""

import argparse
import csv
import os
import re
import struct
import sys

try:
    import xxhash
except ImportError:
    sys.exit("error: the 'xxhash' module is required (pip install xxhash)")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULES = ("default", "palm_normal", "palm_abnormal", "reco_normal", "reco_abnormal",
           "hirow_normal", "hirow_abnormal", "kiniro_normal", "kiniro_abnormal")
EXPECTED_BASE = {"default": 0x82000000}  # the game cores: 0x88000000
CORE_BASE = 0x88000000

SHADER_MAGIC = re.compile(rb"\x10\x2a\x11", re.S)  # as reNut's tool: 0x102A11xx
HEADER_SIZE = 0x24                                 # sizeof(d3d::ShaderContainer)


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def shader_kind(flags):
    return "vs" if flags & 1 else "ps"


def pe_image_base(image):
    """OptionalHeader.ImageBase of the PE image (little-endian headers), or None."""
    if image[:2] != b"MZ" or len(image) < 0x40:
        return None
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if pe + 0x38 > len(image) or image[pe:pe + 4] != b"PE\0\0":
        return None
    if struct.unpack_from("<H", image, pe + 0x18)[0] != 0x10B:  # PE32
        return None
    return struct.unpack_from("<I", image, pe + 0x18 + 0x1C)[0]


def pe_sections(image):
    """[(name, rva, virtual size)]; empty when the headers cannot be read."""
    try:
        pe = struct.unpack_from("<I", image, 0x3C)[0]
        count = struct.unpack_from("<H", image, pe + 6)[0]
        table = pe + 0x18 + struct.unpack_from("<H", image, pe + 0x14)[0]
        out = []
        for i in range(count):
            name, vsize, rva = struct.unpack_from("<8sII", image, table + 40 * i)
            out.append((name.rstrip(b"\0").decode("latin1"), rva, vsize))
        return out
    except struct.error:
        return []


def section_of(sections, rva):
    for name, start, size in sections:
        if start <= rva < start + size:
            return name
    return "?"


def check_header(data, o):
    """reNut's find_shader_headers() test for a candidate at o, as a reason string (None: a container)."""
    if o % 4:
        return "unaligned"
    if o + HEADER_SIZE > len(data):
        return "header past the end of the image"
    virt, phys = struct.unpack_from(">II", data, o + 4)
    ct, dt, so = struct.unpack_from(">III", data, o + 0x10)
    if not 0x24 <= virt < 0x10000:
        return "virtual size 0x%x out of range" % virt
    if not 0 < phys < 0x100000:
        return "physical size 0x%x out of range" % phys
    if o + virt > len(data):
        return "virtual part past the end of the image"
    if ct >= virt or dt >= virt:
        return "constant/definition table offset 0x%x/0x%x not inside the virtual part 0x%x" % (ct, dt, virt)
    if not 0x24 <= so < virt - 8:
        return "shader offset 0x%x not inside the virtual part 0x%x" % (so, virt)
    poff, psize = struct.unpack_from(">II", data, o + so)
    if poff + psize != phys:
        return "shader's physical offset 0x%x + size 0x%x != physical size 0x%x" % (poff, psize, phys)
    return None


def xenosrecomp_scan(blob):
    """The container keys XenosRecomp's directory scan (XenosRecomp/main.cpp) takes from one dumped file."""
    keys = []
    i = 0
    n = len(blob)
    while n > HEADER_SIZE and i < n - HEADER_SIZE - 1:
        flags, virt, phys = struct.unpack_from(">III", blob, i)
        f1c, f20 = struct.unpack_from(">II", blob, i + 0x1C) if i + HEADER_SIZE <= n else (1, 1)
        size = virt + phys
        if (flags & 0xFFFFFF00) == 0x102A1100 and size <= n - i and f1c == 0 and f20 == 0:
            keys.append(xxhash.xxh3_64_intdigest(blob[i:i + size]))
            i += size
        else:
            i += 4
    return keys


def runtime_key(container):
    """renut::gpu::HashShaderContainer: XXH3_64bits(container, virtualSize + physicalSize)."""
    virt, phys = struct.unpack_from(">II", container, 4)
    return xxhash.xxh3_64_intdigest(container[:virt + phys])


def extract_from_image(path, module):
    """(base, results, rejected) for one module image."""
    with open(path, "rb") as fh:
        image = fh.read()
    base = pe_image_base(image)
    expected = EXPECTED_BASE.get(module, CORE_BASE)
    problems = []
    if base is None:
        problems.append("%s: no PE header; assuming image base 0x%08x" % (module, expected))
        base = expected
    elif base != expected:
        problems.append("%s: PE image base 0x%08x, expected 0x%08x (using the PE's)" % (module, base, expected))
    sections = pe_sections(image)

    rejected = []
    headers = []
    for m in SHADER_MAGIC.finditer(image):
        o = m.start()
        why = check_header(image, o)
        if why:
            rejected.append((o, why))
        else:
            headers.append(o)

    results = []
    for i, h in enumerate(headers):
        flags, virt, phys = struct.unpack_from(">III", image, h)
        blob = image[h:h + virt + phys]
        if len(blob) != virt + phys:
            rejected.append((h, "physical part past the end of the image"))
            continue
        f1c, f20 = struct.unpack_from(">II", image, h + 0x1C)
        if f1c or f20:
            rejected.append((h, "fields +0x1C/+0x20 = 0x%x/0x%x (XenosRecomp only takes zero)" % (f1c, f20)))
            continue
        if i + 1 < len(headers) and h + virt + phys > headers[i + 1]:
            rejected.append((h, "extends 0x%x bytes into the next container at +0x%x"
                             % (h + virt + phys - headers[i + 1], headers[i + 1])))
            continue
        results.append({
            "kind": shader_kind(flags),
            "hash": xxhash.xxh3_64_hexdigest(blob),
            "blob": blob,
            "virt": virt,
            "phys": phys,
            "asset": "0x%08x" % (base + h),
            "section": section_of(sections, h),
        })
    rejected.sort()
    return base, results, rejected, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--images", default=os.path.join(ROOT, "out"),
                    help="directory of the <module>_image.bin dumps (default: %(default)s)")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "renut_shader_dump"),
                    help="output directory (default: %(default)s)")
    ap.add_argument("--ucode", action="store_true",
                    help="also write <name>.ucode (physical block only)")
    ap.add_argument("modules", nargs="*", help="modules to scan (default: all nine: %s)" % ", ".join(MODULES))
    args = ap.parse_args()

    modules = args.modules or list(MODULES)
    for m in modules:
        if m not in MODULES:
            sys.exit("unknown module %s (one of %s)" % (m, ", ".join(MODULES)))

    os.makedirs(args.out, exist_ok=True)
    seen = {}       # name -> blob
    manifest = []
    problems = []
    all_rejected = []
    for module in modules:
        path = os.path.join(args.images, "%s_image.bin" % module)
        if not os.path.isfile(path):
            problems.append("%s: no image %s" % (module, path))
            continue
        base, results, rejected, probs = extract_from_image(path, module)
        problems += probs
        all_rejected += [(module, o, why) for o, why in rejected]
        new = 0
        for r in results:
            name = "%s_%s" % (r["kind"], r["hash"])
            manifest.append((name, r["kind"], r["virt"], r["phys"], os.path.basename(path), r["asset"]))
            if name in seen:
                if seen[name] != r["blob"]:
                    problems.append("%s: %s at %s has the key of a different container" % (module, name, r["asset"]))
                continue
            # The key the renderer looks it up by, and the one XenosRecomp files it under.
            key = "%016x" % runtime_key(r["blob"])
            scanned = ["%016x" % k for k in xenosrecomp_scan(r["blob"])]
            if key != r["hash"] or scanned != [r["hash"]]:
                problems.append("%s: %s at %s: runtime key %s, XenosRecomp keys %s"
                                % (module, name, r["asset"], key, scanned))
                continue
            seen[name] = r["blob"]
            new += 1
            with open(os.path.join(args.out, name + ".bin"), "wb") as fh:
                fh.write(r["blob"])
            if args.ucode:
                with open(os.path.join(args.out, name + ".ucode"), "wb") as fh:
                    fh.write(r["blob"][r["virt"]:])
        n_vs = sum(1 for r in results if r["kind"] == "vs")
        secs = sorted({r["section"] for r in results})
        print("%-16s base 0x%08x  %2d vs + %2d ps containers (%s), %2d new, %d rejected candidate(s)"
              % (module, base, n_vs, len(results) - n_vs, ",".join(secs) or "-", new, len(rejected)))

    manifest.sort()
    with open(os.path.join(args.out, "manifest.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["name", "type", "virtual_size", "physical_size", "source", "asset"])
        w.writerows(manifest)

    n_ps = sum(1 for k in seen if k.startswith("ps_"))
    print("\n%d unique shaders (%d ps, %d vs) from %d occurrences -> %s"
          % (len(seen), n_ps, len(seen) - n_ps, len(manifest), args.out))
    stale = sorted(f for f in os.listdir(args.out)
                   if re.fullmatch(r"(ps|vs)_[0-9a-f]{16}\.(bin|ucode)", f) and f.split(".")[0] not in seen)
    if stale:
        problems.append("%d file(s) in %s are not from this dump (XenosRecomp would compile them too): %s"
                        % (len(stale), args.out, ", ".join(stale)))
    if all_rejected:
        print("\n%d rejected candidate(s) (0x102A11xx not a container):" % len(all_rejected))
        for module, o, why in all_rejected:
            print("  %s+0x%06x: %s" % (module, o, why))
    if problems:
        print("\n%d problem(s):" % len(problems))
        for p in problems:
            print("  " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
