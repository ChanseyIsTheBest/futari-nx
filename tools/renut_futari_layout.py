#!/usr/bin/env python3
"""Renut's renderer (banjo-nx's copy, app/src/renut_engine/gpu) for Futari's Direct3D (XDK 2.0.9328): its device-layout
constants and raw device offsets translated from N&B's XDK to Futari's (futari-nx, 2026-10-19; docs/futari-notes.md,
"The deko3d renderer").

How the table was derived: the same functions of both XDKs side by side (tools/d3d_match.py for the pairing, then the
loads and stores of each, e.g. SetIndices, SetRenderTarget, SetDepthStencilSurface, SetStreamSource, SetScissorRect,
SetPixelShader / SetVertexShader, SetPredication, SetVertexDeclaration, CDevice::KickOff / KickOffSegment,
InitializeApiState, InitializePresentationParameters, the hardware-device init and Swap). The public D3DDevice (below
0x2B00: dirty masks, constants, register packets, fence words, flags) is the same; the CDevice-private part has small
insertions, so the shift grows by region:

  N&B offset            Futari = N&B +   pinned by
  < 0x2B00              0                fence tag 0x2A9C, flags 0x2ABC (every setter)
  0x2B00 - 0x2E2C       0xAC             vertex declaration 0x2E2C -> 0x2ED8 (SetVertexDeclaration)
  0x2E30 - 0x33B0       0xB0             the ld/std qword 0x2E30 -> 0x2EE0, 0x2E4C -> 0x2EFC, index buffer 0x3094 ->
                                         0x3144, render targets 0x3098 -> 0x3148, depth 0x30A8 -> 0x3158, strides
                                         0x30F0 -> 0x31A0, scissor 0x3184 -> 0x3234, PS / VS 0x3194 / 0x3198 ->
                                         0x3244 / 0x3248, predication 0x31A4 - 0x31AC -> 0x3254 - 0x325C, tiling
                                         surfaces 0x31B8 -> 0x3268, in-KickOff 0x33B0 -> 0x3460
  0x33B4 - 0x343C       0xC8             KickOff's segment 0x3438 -> 0x3500
  0x3440 - 0x3480       0xCC             0x3440 / 0x3444 -> 0x350C / 0x3510 (KickOffSegment)
  0x3484 - 0x5B30       0xD0             BeginVertices' ring end 0x3484 -> 0x3554, command buffer 0x34CC -> 0x359C,
                                         present interval 0x351C -> 0x35EC, swap count 0x40B0 -> 0x4180, present
                                         fence 0x3A5C -> 0x3B2C, critical sections 0x3A60 / 0x3A7C, 0x541C - 0x5480
  >= 0x5B34             0xD4             0x5B34 -> 0x5C08, behaviour flags 0x5DAC -> 0x5E80 (CreateDevice)

Globals: N&B's qword 0x82F39B38 copied into device+10880 is Futari's 0x821E1AF8 (sub_820AB668); the title process's
VdGlobalDevice import slot 0x820007FC is Futari's 0x820005F8 (the hardware init, sub_820AB6D8). The device allocation
is 0x5F00 bytes (24,320: Direct3D_CreateDevice sub_820A4AC0), N&B's 0x5E00.

  python tools/renut_futari_layout.py [--check]     (idempotent: a translated file carries a marker)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
MARK = '// futari-nx: device offsets translated to Futari\'s XDK (tools/renut_futari_layout.py)'


def shift(x):
    if x < 0x2B00:
        return 0
    if x <= 0x2E2C:
        return 0xAC
    if x <= 0x33B0:
        return 0xB0
    if x < 0x3440:
        return 0xC8
    if x < 0x3484:
        return 0xCC
    if x < 0x5B34:
        return 0xD0
    return 0xD4


def fmt(old_text, value):
    return ('0x%X' % value) if old_text.lower().startswith('0x') else str(value)


def translate(value_text):
    v = int(value_text, 0)
    return fmt(value_text, v + shift(v))


# Constants: name -> exact replacement (when the generic rule is not right, or a value is not an offset).
FIXED = {
    'kDeviceAllocSize': '0x5F00',
    'kGlobalQwordVa': '0x821E1AF8',
    'kVdGlobalDeviceImportSlot': '0x820005F8',
    'kSideBufferSize': None,  # a size, not an offset
    'kRingSinkSize': None, 'kRingSinkGuaranteeMargin': None, 'kRingDataSize': None,
}
CONST = re.compile(r'^(\s*(?:static\s+)?constexpr\s+u32\s+(\w+)\s*=\s*)(0x[0-9A-Fa-f]+|\d+)(u?;.*)$')
CONST_ARRAY = re.compile(r'^(\s*constexpr\s+u32\s+(kDeviceOffsetsMinusOne)\[\]\s*=\s*\{)([^}]*)(\};.*)$')
ACCESS = re.compile(r'\b((?:d|dev|device|dd|src|dst)\.(?:Set)?(?:W|B|H|F|Q|D|U64|S)\()(0x[0-9A-Fa-f]+|\d{4,5})\b')
DEVVA = re.compile(r'\b(d\.va \+ )(0x[0-9A-Fa-f]+)\b')
# Which constants are device offsets: in d3d.h the kDeviceOffset* and kDeviceSnapshotSize; in d3d_native.h the off::
# namespace; in device.cpp the kDeviceOffset* of its own; d3d_native_state.cpp's k* offsets.
OFFSET_NAME = re.compile(r'^(?:kDeviceOffset\w*|kDeviceSnapshotSize|kVertexDeclaration|kVertexShader|kPixelShader)$')


def process(path, in_off_namespace_file=False):
    text = open(path, encoding='utf-8', newline='').read()
    if MARK in text:
        return 0, text
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)
    changes = 0
    in_off = False
    for i, line in enumerate(lines):
        if in_off_namespace_file:
            if re.match(r'^namespace off \{', line):
                in_off = True
            elif in_off and re.match(r'^\}\s*//\s*namespace off', line):
                in_off = False
        m = CONST.match(line)
        if m:
            name = m.group(2)
            if name in FIXED:
                if FIXED[name] is not None and m.group(3) != FIXED[name]:
                    lines[i] = m.group(1) + FIXED[name] + m.group(4)
                    changes += 1
                continue
            if OFFSET_NAME.match(name) or in_off:
                new = translate(m.group(3))
                if new != m.group(3):
                    lines[i] = m.group(1) + new + m.group(4)
                    changes += 1
            continue
        m = CONST_ARRAY.match(line)
        if m:
            vals = [translate(v.strip()) for v in m.group(3).split(',')]
            new = m.group(1) + ', '.join(vals) + m.group(4)
            if new != line:
                lines[i] = new
                changes += 1
            continue
        new = ACCESS.sub(lambda a: a.group(1) + translate(a.group(2)), line)
        new = DEVVA.sub(lambda a: a.group(1) + translate(a.group(2)), new)
        if new != line:
            lines[i] = new
            changes += 1
    out = eol.join(lines)
    # The marker after the first comment block.
    k = out.find('*/')
    out = out[:k + 2] + eol + MARK + out[k + 2:] if k >= 0 else MARK + eol + out
    return changes, out


def main():
    check = '--check' in sys.argv
    files = [('d3d/d3d.h', False), ('d3d/d3d_native.h', True), ('d3d/d3d_native_state.cpp', False),
             ('d3d/hooks/device.cpp', False), ('d3d/hooks/draw.cpp', False), ('d3d/hooks/state.cpp', False)]
    for rel, off_ns in files:
        path = os.path.join(GPU, rel)
        changes, out = process(path, off_ns)
        print('%s: %d lines translated' % (rel, changes))
        if changes and not check:
            open(path, 'w', encoding='utf-8', newline='').write(out)


if __name__ == '__main__':
    main()
