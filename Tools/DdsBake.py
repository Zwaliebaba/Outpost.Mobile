#!/usr/bin/env python3
"""Bakes an uncompressed DDS into a block-compressed one with a full mip chain.

  python Tools/DdsBake.py in.dds out.dds        BC1 (opaque) or BC3 (alpha), decided by the input
  python Tools/DdsBake.py --test                the self-test, stdlib only like every tool here

Why this exists: DdsImage parses BC formats and mip chains and the upload path now consumes them
(Design/CompressedTextures-work-order.md), but nothing in the tree could *produce* one -- runtime
compression is out of scope by the work order, so the compressor is a content tool beside the NMO
codec. The encoder is a simple min/max-endpoint BC1/BC3 and says so: quality is judged by the
slice's two screenshots, not by a metric.

Stdlib only, like everything in Tools/ (AGENTS.md 2).
"""

import argparse
import struct
import sys

DDS_MAGIC = b'DDS '
FOURCC_DX10 = b'DX10'
DXGI_BC1_UNORM = 71
DXGI_BC3_UNORM = 77

# ---------------------------------------------------------------- reading the uncompressed input


def read_uncompressed(data):
    """Returns (width, height, rows of RGBA tuples, has_alpha). Legacy 32-bit masked DDS only --
    which is what every authored texture in this tree is."""
    if data[:4] != DDS_MAGIC:
        raise SystemExit('error: not a DDS file')
    (size, flags, height, width, pitch, depth, mips) = struct.unpack_from('<7I', data, 4)
    if size != 124:
        raise SystemExit('error: malformed header')
    pf_flags = struct.unpack_from('<I', data, 80)[0]
    fourcc = data[84:88] if pf_flags & 0x4 else b''
    rgb_bits = struct.unpack_from('<I', data, 88)[0]
    masks = struct.unpack_from('<4I', data, 92)
    if fourcc:
        raise SystemExit('error: input is already compressed; this tool bakes uncompressed sources')
    if rgb_bits != 32:
        raise SystemExit('error: only 32-bit uncompressed input is baked (found %d bits)' % rgb_bits)

    def shift_of(mask):
        if mask == 0:
            return None
        shift = 0
        while not mask & 1:
            mask >>= 1
            shift += 1
        return shift

    shifts = [shift_of(m) for m in masks]  # r, g, b, a
    has_alpha = bool(pf_flags & 0x1) and masks[3] != 0
    offset = 4 + 124
    pixels = []
    for y in range(height):
        row = []
        for x in range(width):
            (texel,) = struct.unpack_from('<I', data, offset + (y * width + x) * 4)
            r = (texel >> shifts[0]) & 0xFF if shifts[0] is not None else 0
            g = (texel >> shifts[1]) & 0xFF if shifts[1] is not None else 0
            b = (texel >> shifts[2]) & 0xFF if shifts[2] is not None else 0
            a = (texel >> shifts[3]) & 0xFF if has_alpha else 255
            row.append((r, g, b, a))
        pixels.append(row)
    return width, height, pixels, has_alpha


# ---------------------------------------------------------------- mips


def next_mip(pixels):
    """A box filter halving each axis; a 1-wide axis stays 1. Floor dimensions, the D3D12 rule."""
    height = len(pixels)
    width = len(pixels[0])
    out_w = max(1, width // 2)
    out_h = max(1, height // 2)
    out = []
    for y in range(out_h):
        row = []
        for x in range(out_w):
            samples = []
            for dy in (0, 1):
                for dx in (0, 1):
                    sy = min(height - 1, y * 2 + dy)
                    sx = min(width - 1, x * 2 + dx)
                    samples.append(pixels[sy][sx])
            row.append(tuple(sum(s[c] for s in samples) // len(samples) for c in range(4)))
        out.append(row)
    return out


def mip_chain(pixels):
    chain = [pixels]
    while len(chain[-1]) > 1 or len(chain[-1][0]) > 1:
        chain.append(next_mip(chain[-1]))
    return chain


# ---------------------------------------------------------------- BC1 / BC3 blocks


def to_565(rgb):
    return ((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3)


def from_565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def block_texels(pixels, bx, by):
    height = len(pixels)
    width = len(pixels[0])
    texels = []
    for dy in range(4):
        for dx in range(4):
            texels.append(pixels[min(height - 1, by * 4 + dy)][min(width - 1, bx * 4 + dx)])
    return texels


def encode_bc1_block(texels):
    """Min/max endpoints along luminance, four-colour palette, nearest index. Simple on purpose."""
    def luma(t):
        return t[0] * 299 + t[1] * 587 + t[2] * 114

    lo = min(texels, key=luma)
    hi = max(texels, key=luma)
    c0, c1 = to_565(hi[:3]), to_565(lo[:3])
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    if c0 < c1:  # c0 > c1 selects the opaque four-colour mode
        c0, c1 = c1, c0
    p0, p1 = from_565(c0), from_565(c1)
    palette = [p0, p1,
               tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)),
               tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3))]
    indices = 0
    for at, texel in enumerate(texels):
        best = min(range(4), key=lambda i: sum((texel[c] - palette[i][c]) ** 2 for c in range(3)))
        indices |= best << (at * 2)
    return struct.pack('<HHI', c0, c1, indices)


def encode_bc4_alpha_block(texels):
    alphas = [t[3] for t in texels]
    a0, a1 = max(alphas), min(alphas)
    if a0 == a1:
        return struct.pack('<BB6x', a0, a1)
    palette = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    bits = 0
    for at, alpha in enumerate(alphas):
        best = min(range(8), key=lambda i: abs(alpha - palette[i]))
        bits |= best << (at * 3)
    return struct.pack('<BB', a0, a1) + bits.to_bytes(6, 'little')


def encode_level(pixels, has_alpha):
    height = len(pixels)
    width = len(pixels[0])
    out = bytearray()
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            texels = block_texels(pixels, bx, by)
            if has_alpha:
                out += encode_bc4_alpha_block(texels)
            out += encode_bc1_block(texels)
    return bytes(out)


# ---------------------------------------------------------------- writing


def write_dds(path, width, height, levels, has_alpha):
    fmt = DXGI_BC3_UNORM if has_alpha else DXGI_BC1_UNORM
    block_bytes = 16 if has_alpha else 8
    linear = ((width + 3) // 4) * ((height + 3) // 4) * block_bytes
    # DDSD: caps | height | width | pixelformat | mipcount | linearsize
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    caps = 0x1000 | 0x400000 | 0x8  # texture | mipmap | complex
    header = struct.pack('<4s7I44x', DDS_MAGIC, 124, flags, height, width, linear, 0, len(levels))
    pixelformat = struct.pack('<II4s5I', 32, 0x4, FOURCC_DX10, 0, 0, 0, 0, 0)
    caps_block = struct.pack('<4I4x', caps, 0, 0, 0)
    dx10 = struct.pack('<5I', fmt, 3, 0, 1, 0)  # format, TEXTURE2D, no misc, one slice, straight
    with open(path, 'wb') as out:
        out.write(header + pixelformat + caps_block + dx10)
        for level in levels:
            out.write(level)


def bake(in_path, out_path):
    width, height, pixels, has_alpha = read_uncompressed(open(in_path, 'rb').read())
    chain = mip_chain(pixels)
    levels = [encode_level(level, has_alpha) for level in chain]
    write_dds(out_path, width, height, levels, has_alpha)
    src = width * height * 4
    baked = sum(len(l) for l in levels)
    print('%s: %dx%d, %d mips, %s: %d bytes from %d (%.0f%%)' %
          (out_path, width, height, len(chain), 'BC3' if has_alpha else 'BC1', baked, src, 100.0 * baked / src))


# ---------------------------------------------------------------- self-test


def decode_bc1_block(block):
    c0, c1, indices = struct.unpack('<HHI', block)
    p0, p1 = from_565(c0), from_565(c1)
    palette = [p0, p1,
               tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)),
               tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3))]
    return [palette[(indices >> (i * 2)) & 3] for i in range(16)]


def self_test():
    # A gradient with structure on both axes, an awkward non-power-of-two size like the planet map.
    width, height = 20, 12
    pixels = [[(x * 255 // (width - 1), y * 255 // (height - 1), 128, 255) for x in range(width)] for y in range(height)]

    chain = mip_chain(pixels)
    assert [len(c[0]) for c in chain] == [20, 10, 5, 2, 1], [len(c[0]) for c in chain]
    assert [len(c) for c in chain] == [12, 6, 3, 1, 1]

    level = encode_level(pixels, False)
    assert len(level) == 5 * 3 * 8, len(level)

    # Round trip the first block: a smooth gradient must decode within a coarse-quantization bound.
    decoded = decode_bc1_block(level[:8])
    texels = block_texels(pixels, 0, 0)
    worst = max(max(abs(d[c] - t[c]) for c in range(3)) for d, t in zip(decoded, texels))
    assert worst <= 40, worst

    alpha_level = encode_level([[(255, 0, 0, x * 255 // (width - 1)) for x in range(width)] for y in range(height)], True)
    assert len(alpha_level) == 5 * 3 * 16, len(alpha_level)

    # The written file parses back by the same rules DdsImage applies: magic, DX10 fourCC, counts.
    import io, os, tempfile
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'test.dds')
        write_dds(out, width, height, [level] * len(chain), False)
        data = open(out, 'rb').read()
        assert data[:4] == DDS_MAGIC and data[84:88] == FOURCC_DX10
        assert struct.unpack_from('<I', data, 28)[0] == len(chain)  # mip count
        assert struct.unpack_from('<I', data, 128)[0] == DXGI_BC1_UNORM
    print('DdsBake self-test: ok')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('paths', nargs='*', help='in.dds out.dds')
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    if args.test:
        self_test()
        return
    if len(args.paths) != 2:
        parser.error('expected: in.dds out.dds (or --test)')
    bake(args.paths[0], args.paths[1])


if __name__ == '__main__':
    main()
