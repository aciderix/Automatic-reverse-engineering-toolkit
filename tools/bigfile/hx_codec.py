#!/usr/bin/env python3
"""
hx_codec.py — decoder for the Opal/Zouna "Hx" texture codec (the compressed
pixel container inside Texture_Z nodes; see docs/OPAL_BIGFILE_FORMAT.md).

Reverse engineered from the engine; the per-stream decoders below are validated
by exact sub-block consumption on real textures (each decodes its declared
entry count and consumes its sub-block to the byte).

Structure of an Hx blob:
  * a 0x26-byte node-level header (handled by opal_nodes.parse_texture)
  * an Hx header with width/height/format and, big-endian, the (offset,size,
    count) of five sub-streams:
      - block-index stream  @ (be16@0x41 size, be24@0x43 off)   [1 table]
      - alpha_idx palette   @ (be16@0x27, be24@0x21, be24@0x24) [2 tables, 6 deltas/entry -> u32]
      - alpha_end palette   @ (be16@0x2f, be24@0x29, be24@0x2c) [1 table, 2-D delta]
      - color_end palette   @ (be16@0x37, be24@0x31, be24@0x34) [1 table, 2 bytes/entry]
      - color_idx palette   @ (be16@0x3f, be24@0x39, be24@0x3c) [1 table, 2-D delta]
  * Each stream: a DEFLATE-dynamic Huffman table (build_table) then delta-coded
    entries. The block-index stream selects, per BC block, palette entries.

Engine refs: bit reader sub_95a240; Huffman sub_959480; table builder sub_95a020;
channels sub_959590/9596d0/959a20/959ca0; assembly sub_958510.
"""
import struct

PERM = [17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16]
# 3-bit field remap used when packing colour/alpha index nibbles (engine @0xf3ce80)
REMAP_F3CE80 = [0, 2, 3, 4, 5, 6, 7, 1, 1, 0, 5, 4, 3, 2, 6, 7]

def be(d, o, n):
    v = 0
    for i in range(n):
        v = (v << 8) | d[o + i]
    return v

class BitReader:
    """MSB-first bit reader over a byte buffer (engine sub_95a240)."""
    def __init__(self, data):
        self.d = data; self.p = 0; self.buf = 0; self.n = 0
    def bits(self, k):
        if k == 0:
            return 0
        while self.n < k:
            b = self.d[self.p] if self.p < len(self.d) else 0
            self.p += 1
            self.buf = (self.buf | (b << (24 - self.n))) & 0xffffffff
            self.n += 8
        out = self.buf >> (32 - k)
        self.buf = (self.buf << k) & 0xffffffff
        self.n -= k
        return out

def canonical(lengths):
    maxlen = max(lengths) if lengths else 0
    if maxlen == 0:
        return None
    bl_count = [0] * (maxlen + 1)
    for l in lengths:
        if l:
            bl_count[l] += 1
    code = 0
    next_code = [0] * (maxlen + 1)
    for b in range(1, maxlen + 1):
        code = (code + bl_count[b - 1]) << 1
        next_code[b] = code
        if code > (1 << b):
            return None
    table = {}
    for sym, l in enumerate(lengths):
        if l:
            table[(l, next_code[l])] = sym
            next_code[l] += 1
    return table, maxlen

def huff_decode(br, table_maxlen):
    table, maxlen = table_maxlen
    code = 0
    for l in range(1, maxlen + 1):
        code = (code << 1) | br.bits(1)
        if (l, code) in table:
            return table[(l, code)]
    raise ValueError("invalid Huffman code")

def build_table(br):
    """DEFLATE-dynamic Huffman table from the bitstream (engine sub_95a020).
    Returns (table, maxlen)."""
    N = br.bits(14)
    if N == 0 or N > 0x2000:
        raise ValueError(f"bad alphabet size N={N}")
    M = br.bits(5)
    if not (1 <= M <= 21):
        raise ValueError(f"bad HCLEN M={M}")
    meta_len = [0] * 21
    for i in range(M):
        meta_len[PERM[i]] = br.bits(3)
    meta = canonical(meta_len)
    if meta is None:
        raise ValueError("meta table over-subscribed")
    lens, prev = [], 0
    while len(lens) < N:
        s = huff_decode(br, meta)
        if s <= 16:
            lens.append(s); prev = s
        elif s == 17:
            lens += [0] * (br.bits(3) + 3)
        elif s == 18:
            lens += [0] * (br.bits(7) + 11)
        elif s == 19:
            lens += [prev] * (br.bits(2) + 3)
        elif s == 20:
            lens += [prev] * (br.bits(6) + 7)
        else:
            raise ValueError(f"bad code-length symbol {s}")
    main = canonical(lens[:N])
    if main is None:
        raise ValueError("main table over-subscribed")
    return main

def _grid(half):
    """2-D offset grid: index -> (dx,dy), dx/dy in [-half,half] (engine init)."""
    side = 2 * half + 1
    dx, dy = [], []
    v, w = -half, -half
    for _ in range(side * side):
        dx.append(v); dy.append(w); v += 1
        if v > half:
            v = -half; w += 1
    return dx, dy

# --- per-stream decoders (validated by exact sub-block consumption) ---------

def decode_color_end(br, count):
    """sub_959590: count entries, two running-sum (mod256) bytes each."""
    a = b = 0; out = []
    for _ in range(count):
        a = (a + huff_decode(br, br.tbl)) & 0xff
        b = (b + huff_decode(br, br.tbl)) & 0xff
        out.append((a, b))
    return out

def stream(d, off, size, ntab):
    br = BitReader(d[off:off + size])
    tabs = [build_table(br) for _ in range(ntab)]
    return br, tabs

def decode_palettes(hx):
    """Decode the four palettes + the per-block index symbols. Returns a dict.
    Output packing into final BC bytes / assembly is the remaining step."""
    res = {}
    w, h = be(hx, 0x0c, 2), be(hx, 0x0e, 2)
    blocks = ((w + 3) // 4) * ((h + 3) // 4)
    res["w"], res["h"], res["blocks"] = w, h, blocks

    # block-index stream
    br, (tab,) = stream(hx, be(hx, 0x43, 3), be(hx, 0x41, 2), 1)
    res["block_syms"] = [huff_decode(br, tab) for _ in range(blocks)]

    # color endpoints: 1 table, 2 delta bytes / entry
    br, (tab,) = stream(hx, be(hx, 0x31, 3), be(hx, 0x34, 3), 1)
    a = b = 0; ce = []
    for _ in range(be(hx, 0x37, 2)):
        a = (a + huff_decode(br, tab)) & 0xff
        b = (b + huff_decode(br, tab)) & 0xff
        ce.append((a, b))
    res["color_end"] = ce

    # alpha indices: 2 tables, 6 deltas (5/6-bit) / entry
    br, (t1, t2) = stream(hx, be(hx, 0x21, 3), be(hx, 0x24, 3), 2)
    dt = [0] * 6; ai = []
    order = [(0, t1, 0x1f), (1, t2, 0x3f), (2, t1, 0x1f),
             (3, t1, 0x1f), (4, t2, 0x3f), (5, t1, 0x1f)]
    for _ in range(be(hx, 0x27, 2)):
        for i, t, m in order:
            dt[i] = (dt[i] + huff_decode(br, t)) & m
        ai.append(tuple(dt))
    res["alpha_idx"] = ai

    # color indices: 1 table, 2-D delta (15×15 grid), 8 states / entry
    gdx, gdy = _grid(7)
    br, (tab,) = stream(hx, be(hx, 0x39, 3), be(hx, 0x3c, 3), 1)
    st = [[0, 0] for _ in range(8)]; ci = []
    for _ in range(be(hx, 0x3f, 2)):
        e = []
        for j in range(8):
            s = huff_decode(br, tab)
            st[j][0] = (gdx[s] + st[j][0]) & 7
            st[j][1] = (gdy[s] + st[j][1]) & 7
            e.append((st[j][0], st[j][1]))
        ci.append(tuple(e))
    res["color_idx"] = ci

    # alpha endpoints: 1 table, 2-D delta (7×7 grid)
    gdx, gdy = _grid(3)
    br, (tab,) = stream(hx, be(hx, 0x29, 3), be(hx, 0x2c, 3), 1)
    st = [[0, 0] for _ in range(8)]; ae = []
    for _ in range(be(hx, 0x2f, 2)):
        e = []
        for j in range(8):
            s = huff_decode(br, tab)
            st[j][0] = (gdx[s] + st[j][0]) & 3
            st[j][1] = (gdy[s] + st[j][1]) & 3
            e.append((st[j][0], st[j][1]))
        ae.append(tuple(e))
    res["alpha_end"] = ae
    return res

if __name__ == "__main__":
    import sys
    hx = open(sys.argv[1], "rb").read()
    r = decode_palettes(hx)
    print(f"{r['w']}x{r['h']}  {r['blocks']} blocks")
    print(f"  block_syms: {len(r['block_syms'])}  range[{min(r['block_syms'])},{max(r['block_syms'])}]")
    for k in ("color_end", "alpha_idx", "color_idx", "alpha_end"):
        print(f"  {k}: {len(r[k])} entries")
