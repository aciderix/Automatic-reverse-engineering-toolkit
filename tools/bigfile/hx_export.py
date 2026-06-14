#!/usr/bin/env python3
"""
hx_export.py — full decoder for the Opal/Zouna "Hx" texture codec: decode an
Hx blob to a DXT5 surface and export PNG. Reverse-engineered end to end
(see docs/OPAL_BIGFILE_FORMAT.md); the exact per-stream packings and the
sub_95b160 block assembly were recovered with Ghidra's decompiler.

Validated: the per-block selector stream is consumed to the byte on every
texture, and large textures decode to clean images (e.g. 1280x720 is pixel-
smooth). Some small textures use a format variant still being finalised.

Usage:  hx_export.py <texture.hx> <out.png>
"""
import sys, glob, os
sys.path.insert(0,'/home/user/Automatic-reverse-engineering-toolkit/tools/bigfile')
import hx_codec as H
from PIL import Image

RC=[0,2,3,4,5,6,7,1,1,0,5,4,3,2,6,7]     # 0xf3ce80 (color, 3-bit)
RA=[0,2,3,1]                              # 0xf3ce7c (alpha, 2-bit)
CNT=[1,2,2,3,3,3,3,4]
RAW=[0,0,0,0, 0,0,1,1, 0,1,0,1, 0,0,1,2, 1,2,0,0, 0,1,0,2, 1,0,2,0, 0,1,2,3]

def tabs(hx,off,size,n):
    br=H.BitReader(hx[off:off+size]); return br,[H.build_table(br) for _ in range(n)]

def pal_color(hx):   # 0x10c  u16
    br,(t,)=tabs(hx,H.be(hx,0x31,3),H.be(hx,0x34,3),1); a=b=0;o=[]
    for _ in range(H.be(hx,0x37,2)):
        a=(a+H.huff_decode(br,t))&0xff;b=(b+H.huff_decode(br,t))&0xff;o.append((b<<8)|a)
    return o
def pal_alpha(hx):   # 0xec  u32
    br,(t1,t2)=tabs(hx,H.be(hx,0x21,3),H.be(hx,0x24,3),2); d=[0]*6;o=[]
    for _ in range(H.be(hx,0x27,2)):
        d[0]=(d[0]+H.huff_decode(br,t1))&0x1f; d[1]=(d[1]+H.huff_decode(br,t2))&0x3f
        d[2]=(d[2]+H.huff_decode(br,t1))&0x1f; d[3]=(d[3]+H.huff_decode(br,t1))&0x1f
        d[4]=(d[4]+H.huff_decode(br,t2))&0x3f; d[5]=(d[5]+H.huff_decode(br,t1))&0x1f
        v=((((d[3]<<6|d[4])<<5|d[5])<<5|d[0])<<6|d[1])<<5|d[2]
        o.append(v&0xffffffff)
    return o
def pal_ep(hx):      # 0x11c  (u16,u16,u16); 8 states mod8 grid15, remap RC
    gdx,gdy=H._grid(7); st=[0]*16
    br,(t,)=tabs(hx,H.be(hx,0x39,3),H.be(hx,0x3c,3),1); o=[]
    for _ in range(H.be(hx,0x3f,2)):
        for j in range(8):
            s=H.huff_decode(br,t); st[j*2]=(gdx[s]+st[j*2])&7; st[j*2+1]=(gdy[s]+st[j*2+1])&7
        L=[RC[st[k]] for k in range(16)]
        u0=((((L[4]|L[5]<<3)<<3|L[3])<<3|L[2])<<3|L[1])<<3|L[0]
        u1=(((((L[9]|L[10]<<3)<<3|L[8])<<3|L[7])<<3|L[6])<<2)|(L[5]>>1)
        u2=(((((L[15]<<3|L[14])<<3|L[13])<<3|L[12])<<3|L[11])<<1)|(L[10]>>2)
        o.append((u0&0xffff,u1&0xffff,u2&0xffff))
    return o
def pal_aidx(hx):    # 0xfc  u32; 8 states mod4 grid7, remap RA
    gdx,gdy=H._grid(3); st=[0]*16
    br,(t,)=tabs(hx,H.be(hx,0x29,3),H.be(hx,0x2c,3),1); o=[]
    for _ in range(H.be(hx,0x2f,2)):
        for j in range(8):
            s=H.huff_decode(br,t); st[j*2]=(gdx[s]+st[j*2])&3; st[j*2+1]=(gdy[s]+st[j*2+1])&3
        v=0
        for k in range(15,-1,-1): v=(v<<2)|RA[st[k]]
        o.append(v&0xffffffff)
    return o

def index_tables(hx):
    br,_=tabs(hx,H.be(hx,0x43,3),H.be(hx,0x41,2),0)
    T={0x74:H.build_table(br)}
    if H.be(hx,0x27,2): T[0x8c]=H.build_table(br); T[0xbc]=H.build_table(br)
    if H.be(hx,0x37,2): T[0xa4]=H.build_table(br); T[0xd4]=H.build_table(br)
    return T

def decode(hx):
    w,h=H.be(hx,0x0c,2),H.be(hx,0x0e,2)
    color=pal_color(hx); alpha=pal_alpha(hx); ep=pal_ep(hx); aidx=pal_aidx(hx)
    nC,nA,nE,nI=len(color),len(alpha),len(ep),len(aidx)
    T=index_tables(hx)
    so=H.be(hx,0x43,3)+H.be(hx,0x41,2)
    br=H.BitReader(hx[so:])
    bw,bh=(w+3)//4,(h+3)//4
    mbw,mbh=(bw+1)//2,(bh+1)//2
    blocks=[None]*(bw*bh)
    aC=aA=aE=aI=0; sym=1
    for mrow in range(mbh):
        cols=range(mbw) if (mrow&1)==0 else range(mbw-1,-1,-1)
        for mcol in cols:
            if sym==1: sym=H.huff_decode(br,T[0x74])|0x200
            cat=sym&7; cnt=CNT[cat]; sym>>=3
            cB=[0]*cnt; aB=[0]*cnt
            for i in range(cnt):
                aC=(aC+H.huff_decode(br,T[0xa4]))%nC; cB[i]=color[aC]
            for i in range(cnt):
                aA=(aA+H.huff_decode(br,T[0x8c]))%nA; aB[i]=alpha[aA]
            for sr in range(2):
                for sc in range(2):
                    aE=(aE+H.huff_decode(br,T[0xd4]))%nE
                    aI=(aI+H.huff_decode(br,T[0xbc]))%nI
                    e=RAW[cat*4+sr*2+sc]
                    e0,e1,e2=ep[aE]
                    d0=((e0<<16)|cB[e])&0xffffffff
                    d1=((e2<<16)|e1)&0xffffffff
                    d2=aB[e]; d3=aidx[aI]
                    bx,by=mcol*2+sc, mrow*2+sr
                    if bx<bw and by<bh:
                        # standard DXT5: alpha(d2,d3) then color(d0,d1)
                        import struct
                        blk=struct.pack('<IIII',d2,d3,d0,d1)
                        blocks[by*bw+bx]=blk
    return w,h,bw,bh,blocks,br.p,len(br.d)

# DXT5 decode to RGBA
import struct
def dxt5_block(blk):
    a0,a1=blk[0],blk[1]
    abits=int.from_bytes(blk[2:8],'little')
    al=[]
    if a0>a1: at=[a0,a1]+[((7-i)*a0+i*a1)//7 for i in range(1,7)]
    else: at=[a0,a1]+[((5-i)*a0+i*a1)//5 for i in range(1,5)]+[0,255]
    for i in range(16): al.append(at[(abits>>(3*i))&7])
    c0,c1=struct.unpack('<HH',blk[8:12]); cidx=struct.unpack('<I',blk[12:16])[0]
    def rgb(c): return ((c>>11&31)*255//31,(c>>5&63)*255//63,(c&31)*255//31)
    r0,g0,b0=rgb(c0); r1,g1,b1=rgb(c1)
    pal=[(r0,g0,b0),(r1,g1,b1)]
    if c0>c1: pal+=[((2*r0+r1)//3,(2*g0+g1)//3,(2*b0+b1)//3),((r0+2*r1)//3,(g0+2*g1)//3,(b0+2*b1)//3)]
    else: pal+=[((r0+r1)//2,(g0+g1)//2,(b0+b1)//2),(0,0,0)]
    px=[]
    for i in range(16):
        c=pal[(cidx>>(2*i))&3]; px.append((c[0],c[1],c[2],al[i]))
    return px


def export(hx_path, out_path):
    hx=open(hx_path,'rb').read()
    w,h,bw,bh,blocks,used,tot=decode(hx)
    img=Image.new('RGBA',(bw*4,bh*4))
    for byi in range(bh):
        for bxi in range(bw):
            blk=blocks[byi*bw+bxi]
            if not blk: continue
            px=dxt5_block(blk)
            for i in range(16):
                img.putpixel((bxi*4+i%4, byi*4+i//4), px[i])
    img=img.crop((0,0,w,h))
    img.save(out_path)            # RGBA PNG (alpha preserved)
    print(f"{w}x{h} decoded (selector stream {used}/{tot}) -> {out_path}")

if __name__=='__main__':
    import sys
    export(sys.argv[1], sys.argv[2])
