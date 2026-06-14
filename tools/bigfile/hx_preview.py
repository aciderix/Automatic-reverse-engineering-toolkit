#!/usr/bin/env python3
"""
hx_preview.py — low-resolution colour preview of an Hx texture.

Proof that the Hx decode pipeline (hx_codec.py) yields real image data: it
renders one dominant colour per 2×2 macroblock, taken from the decoded
`color_end` palette (RGB565) indexed by the accumulated colour-endpoint
selectors. The output is a coherent (low adjacent-pixel difference), recognisable
thumbnail — not noise — confirming the entropy decode is correct.

This is NOT full resolution: the exact per-block endpoint/index assignment
(engine sub_95b160) is still being finalised, so per-pixel colour/alpha indices
are not yet applied. See docs/OPAL_BIGFILE_FORMAT.md.

Usage:  hx_preview.py <texture.hx> <out.png>
"""
import sys
import hx_codec as H

def _rgb565(v):
    return ((v >> 11 & 0x1f) * 255 // 31,
            (v >> 5 & 0x3f) * 255 // 63,
            (v & 0x1f) * 255 // 31)

def color_end_palette(hx):
    """Decode the color_end stream into a list of RGB565 u16 values."""
    br, (tab,) = H.stream(hx, H.be(hx, 0x31, 3), H.be(hx, 0x34, 3), 1)
    a = b = 0
    out = []
    for _ in range(H.be(hx, 0x37, 2)):
        a = (a + H.huff_decode(br, tab)) & 0xff
        b = (b + H.huff_decode(br, tab)) & 0xff
        out.append(a | (b << 8))
    return out

def preview(hx_path, out_path):
    from PIL import Image
    hx = open(hx_path, "rb").read()
    w, h = H.be(hx, 0x0c, 2), H.be(hx, 0x0e, 2)
    pal = color_end_palette(hx)
    macroblocks, _, _ = H.decode_selectors(hx)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    mbw, mbh = (bw + 1) // 2, (bh + 1) // 2
    img = Image.new("RGB", (mbw, mbh))
    idx = 0
    for i, (cnt, ce_sel, ae_sel, idx_sel) in enumerate(macroblocks):
        if pal:
            idx = (idx + ce_sel[0]) % len(pal)
            img.putpixel((i % mbw, i // mbw), _rgb565(pal[idx]))
    scale = max(1, 256 // max(mbw, mbh))
    img.resize((mbw * scale, mbh * scale), Image.NEAREST).save(out_path)
    print(f"{w}x{h}  {len(pal)} colours  {len(macroblocks)} macroblocks -> {out_path}")

if __name__ == "__main__":
    preview(sys.argv[1], sys.argv[2])
