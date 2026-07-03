//! IR → compilable C emitter (roadmap §7), first milestone: goto-based C that
//! recompiles. Structured emission (reusing `structure`) and typed variables
//! come later; this closes the loop enough to start the verification harness
//! (§8, "recompiles" level).
//!
//! Pipeline: optimized SSA IR → SSA destruction (φ lowering) → C text.
//! Every value becomes a `uint64_t`; unmodelled instructions are emitted as
//! comments (their clobbers were already `Undef`), so the output compiles.
#![allow(dead_code)]

pub mod llvm;
pub mod structured;

use crate::ir::types::*;
use std::collections::BTreeSet;
use std::fmt::Write;

/// Lower φ-nodes out of SSA by inserting, on every incoming edge, a small block
/// that copies each φ argument into the φ's value, then jumps to the join.
/// Inserting on *every* edge (not only critical ones) keeps it simple and
/// correct: copies always live in a dedicated single-pred/single-succ block.
pub(crate) fn destruct_ssa(func: &mut IrFunction) {
    let n = func.blocks.len();
    let mut next_id = func.blocks.iter().map(|b| b.id).max().unwrap_or(0) + 1;
    let mut new_blocks: Vec<Block> = Vec::new();

    for s in 0..n {
        let phis: Vec<(ValueId, Vec<ValueId>)> = func.blocks[s]
            .stmts
            .iter()
            .filter_map(|st| match st {
                Stmt::Assign { dst, expr: Expr::Phi(args) } => Some((*dst, args.clone())),
                _ => None,
            })
            .collect();
        if phis.is_empty() {
            continue;
        }
        let preds = func.blocks[s].pred.clone();
        let s_id = func.blocks[s].id;
        for (i, &p) in preds.iter().enumerate() {
            let mid = next_id;
            next_id += 1;
            let mut stmts: Vec<Stmt> = phis
                .iter()
                .map(|(dst, args)| Stmt::Assign {
                    dst: *dst,
                    expr: Expr::Use(args[i]),
                })
                .collect();
            stmts.push(Stmt::Jump(BlockId(s_id)));
            new_blocks.push(Block {
                id: mid,
                addr: 0,
                stmts,
                succ: vec![s_id],
                pred: vec![p],
            });
            // Redirect predecessor p's terminator edge s_id -> mid.
            let pb = func.blocks.iter_mut().find(|b| b.id == p).unwrap();
            for su in pb.succ.iter_mut() {
                if *su == s_id {
                    *su = mid;
                }
            }
            redirect_terminator(pb.stmts.last_mut(), s_id, mid);
        }
        // The join's predecessors are now the new copy blocks; drop its φs.
        func.blocks[s].pred = (0..preds.len() as u32).map(|k| next_id - preds.len() as u32 + k).collect();
        func.blocks[s]
            .stmts
            .retain(|st| !matches!(st, Stmt::Assign { expr: Expr::Phi(_), .. }));
    }
    func.blocks.extend(new_blocks);
}

fn redirect_terminator(last: Option<&mut Stmt>, from: u32, to: u32) {
    if let Some(st) = last {
        match st {
            Stmt::Jump(b) => {
                if b.0 == from {
                    b.0 = to;
                }
            }
            Stmt::Branch { taken, fallthrough, .. } => {
                if taken.0 == from {
                    taken.0 = to;
                }
                if fallthrough.0 == from {
                    fallthrough.0 = to;
                }
            }
            Stmt::Switch { cases, default, .. } => {
                for (_, b) in cases.iter_mut() {
                    if b.0 == from {
                        b.0 = to;
                    }
                }
                if default.0 == from {
                    default.0 = to;
                }
            }
            _ => {}
        }
    }
}

/// Scalar SSE float runtime helpers. XMM scalar values are carried through the
/// integer IR as bit patterns in `uint64_t`; these reinterpret the bits, compute
/// with native (IEEE-754) `float`/`double`, and reinterpret back, so the
/// decompiled floating-point arithmetic is bit-exact with the original binary.
pub(crate) const FLOAT_HELPERS: &str = concat!(
    "typedef union{uint32_t u;float f;}__fp32;typedef union{uint64_t u;double d;}__fp64;\n",
    "static inline uint64_t __fp_f32(float f){__fp32 t;t.f=f;return t.u;}\n",
    "static inline float __fp_g32(uint64_t b){__fp32 t;t.u=(uint32_t)b;return t.f;}\n",
    "static inline uint64_t __fp_f64(double d){__fp64 t;t.d=d;return t.u;}\n",
    "static inline double __fp_g64(uint64_t b){__fp64 t;t.u=b;return t.d;}\n",
    "static inline uint64_t __fp_add32(uint64_t a,uint64_t b){return __fp_f32(__fp_g32(a)+__fp_g32(b));}\n",
    "static inline uint64_t __fp_sub32(uint64_t a,uint64_t b){return __fp_f32(__fp_g32(a)-__fp_g32(b));}\n",
    "static inline uint64_t __fp_mul32(uint64_t a,uint64_t b){return __fp_f32(__fp_g32(a)*__fp_g32(b));}\n",
    "static inline uint64_t __fp_div32(uint64_t a,uint64_t b){return __fp_f32(__fp_g32(a)/__fp_g32(b));}\n",
    "static inline uint64_t __fp_add64(uint64_t a,uint64_t b){return __fp_f64(__fp_g64(a)+__fp_g64(b));}\n",
    "static inline uint64_t __fp_sub64(uint64_t a,uint64_t b){return __fp_f64(__fp_g64(a)-__fp_g64(b));}\n",
    "static inline uint64_t __fp_mul64(uint64_t a,uint64_t b){return __fp_f64(__fp_g64(a)*__fp_g64(b));}\n",
    "static inline uint64_t __fp_div64(uint64_t a,uint64_t b){return __fp_f64(__fp_g64(a)/__fp_g64(b));}\n",
    "static inline uint64_t __fp_i32_32(uint64_t i){return __fp_f32((float)(int32_t)i);}\n",
    "static inline uint64_t __fp_i64_32(uint64_t i){return __fp_f32((float)(int64_t)i);}\n",
    "static inline uint64_t __fp_i32_64(uint64_t i){return __fp_f64((double)(int32_t)i);}\n",
    "static inline uint64_t __fp_i64_64(uint64_t i){return __fp_f64((double)(int64_t)i);}\n",
    "static inline uint64_t __fp_32_i32(uint64_t a){return (uint64_t)(int64_t)(int32_t)__fp_g32(a);}\n",
    "static inline uint64_t __fp_32_i64(uint64_t a){return (uint64_t)(int64_t)__fp_g32(a);}\n",
    "static inline uint64_t __fp_64_i32(uint64_t a){return (uint64_t)(int64_t)(int32_t)__fp_g64(a);}\n",
    "static inline uint64_t __fp_64_i64(uint64_t a){return (uint64_t)(int64_t)__fp_g64(a);}\n",
    "static inline uint64_t __fp_32_64(uint64_t a){return __fp_f64((double)__fp_g32(a));}\n",
    "static inline uint64_t __fp_64_32(uint64_t a){return __fp_f32((float)__fp_g64(a));}\n",
    "static inline uint64_t __fp_lt32(uint64_t a,uint64_t b){return __fp_g32(a)<__fp_g32(b);}\n",
    "static inline uint64_t __fp_eq32(uint64_t a,uint64_t b){return __fp_g32(a)==__fp_g32(b);}\n",
    "static inline uint64_t __fp_un32(uint64_t a,uint64_t b){float x=__fp_g32(a),y=__fp_g32(b);return x!=x||y!=y;}\n",
    "static inline uint64_t __fp_lt64(uint64_t a,uint64_t b){return __fp_g64(a)<__fp_g64(b);}\n",
    "static inline uint64_t __fp_eq64(uint64_t a,uint64_t b){return __fp_g64(a)==__fp_g64(b);}\n",
    "static inline uint64_t __fp_un64(uint64_t a,uint64_t b){double x=__fp_g64(a),y=__fp_g64(b);return x!=x||y!=y;}\n",
    // Packed integer lanes (two 32-bit lanes per 64-bit half), no carry across lanes.
    "static inline uint64_t __pi_add32(uint64_t a,uint64_t b){uint32_t l=(uint32_t)a+(uint32_t)b,h=(uint32_t)(a>>32)+(uint32_t)(b>>32);return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __pi_sub32(uint64_t a,uint64_t b){uint32_t l=(uint32_t)a-(uint32_t)b,h=(uint32_t)(a>>32)-(uint32_t)(b>>32);return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __pi_eq32(uint64_t a,uint64_t b){uint32_t l=(uint32_t)a==(uint32_t)b?~0u:0,h=(uint32_t)(a>>32)==(uint32_t)(b>>32)?~0u:0;return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __pi_gt32(uint64_t a,uint64_t b){uint32_t l=(int32_t)a>(int32_t)b?~0u:0,h=(int32_t)(a>>32)>(int32_t)(b>>32)?~0u:0;return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __pi_shuf_lo(uint64_t lo,uint64_t hi,uint64_t m){uint64_t a=((m&3)<2?lo:hi)>>(((m&3)&1)*32),b=(((m>>2)&3)<2?lo:hi)>>((((m>>2)&3)&1)*32);return (a&0xffffffff)|((b&0xffffffff)<<32);}\n",
    // pshuflw/pshufhw: shuffle the four 16-bit words within one 64-bit half per the
    // immediate (2 bits select each source word) — used by the SSE2 broadcast
    // idiom (`movd; pshuflw ...,0; pshufd ...,0`).
    "static inline uint64_t __pi_shufw(uint64_t x,uint32_t imm){uint64_t r=0;for(int i=0;i<4;i++){uint16_t w=(uint16_t)(x>>(((imm>>(i*2))&3)*16));r|=(uint64_t)w<<(i*16);}return r;}\n",
    "static inline uint64_t __pi_shuf_hi(uint64_t lo,uint64_t hi,uint64_t m){uint64_t a=(((m>>4)&3)<2?lo:hi)>>((((m>>4)&3)&1)*32),b=(((m>>6)&3)<2?lo:hi)>>((((m>>6)&3)&1)*32);return (a&0xffffffff)|((b&0xffffffff)<<32);}\n",
    "static inline uint64_t __pi_add16(uint64_t a,uint64_t b){uint64_t r=0;for(int i=0;i<64;i+=16)r|=(uint64_t)(uint16_t)((a>>i)+(b>>i))<<i;return r;}\n",
    // Unpack-low helpers (apply to the high halves to get the unpack-high ops).
    "static inline uint64_t __pi_unpcklwd_lo(uint64_t d,uint64_t s){return (uint64_t)(uint16_t)d|((uint64_t)(uint16_t)s<<16)|((uint64_t)(uint16_t)(d>>16)<<32)|((uint64_t)(uint16_t)(s>>16)<<48);}\n",
    "static inline uint64_t __pi_unpcklwd_hi(uint64_t d,uint64_t s){return (uint64_t)(uint16_t)(d>>32)|((uint64_t)(uint16_t)(s>>32)<<16)|((uint64_t)(uint16_t)(d>>48)<<32)|((uint64_t)(uint16_t)(s>>48)<<48);}\n",
    "static inline uint64_t __pi_unpckldq_lo(uint64_t d,uint64_t s){return (d&0xffffffff)|((s&0xffffffff)<<32);}\n",
    "static inline uint64_t __pi_unpckldq_hi(uint64_t d,uint64_t s){return (d>>32)|((s>>32)<<32);}\n",
    "static inline uint64_t __pi_gt16(uint64_t a,uint64_t b){uint64_t r=0;for(int i=0;i<64;i+=16)r|=(uint64_t)((int16_t)(a>>i)>(int16_t)(b>>i)?0xffffu:0)<<i;return r;}\n",
    // Packed compare, byte/word granularity (one 64-bit half at a time), used by
    // the SSE2 string scanners (strlen/memchr: `pcmpeqb` + `pmovmskb`).
    "static inline uint64_t __pi_eq8(uint64_t a,uint64_t b){uint64_t r=0;for(int i=0;i<64;i+=8)r|=(uint64_t)((uint8_t)(a>>i)==(uint8_t)(b>>i)?0xffu:0)<<i;return r;}\n",
    "static inline uint64_t __pi_eq16(uint64_t a,uint64_t b){uint64_t r=0;for(int i=0;i<64;i+=16)r|=(uint64_t)((uint16_t)(a>>i)==(uint16_t)(b>>i)?0xffffu:0)<<i;return r;}\n",
    "static inline uint64_t __pi_gt8(uint64_t a,uint64_t b){uint64_t r=0;for(int i=0;i<64;i+=8)r|=(uint64_t)((int8_t)(a>>i)>(int8_t)(b>>i)?0xffu:0)<<i;return r;}\n",
    // pmovmskb: the high bit of each of the 16 bytes -> a 16-bit mask (low half
    // bytes 0..7, high half bytes 8..15).
    "static inline uint32_t __pi_mskb(uint64_t lo,uint64_t hi){uint32_t m=0;for(int i=0;i<8;i++){m|=((uint32_t)((lo>>(i*8+7))&1))<<i;m|=((uint32_t)((hi>>(i*8+7))&1))<<(i+8);}return m;}\n",
    "static inline uint64_t __pi_muludq(uint64_t a,uint64_t b){return (uint64_t)(uint32_t)a*(uint32_t)b;}\n",
    "static inline uint64_t __pi_subus16(uint64_t a,uint64_t b){uint64_t r=0;for(int i=0;i<64;i+=16){uint16_t x=(uint16_t)(a>>i),y=(uint16_t)(b>>i);r|=(uint64_t)(uint16_t)(x>y?x-y:0)<<i;}return r;}\n",
    // Packed single-precision (4 floats per 128-bit reg, 2 per 64-bit half),
    // bit-exact via native IEEE-754 float per lane.
    "static inline uint64_t __ps_add(uint64_t a,uint64_t b){uint32_t l=(uint32_t)__fp_f32(__fp_g32(a)+__fp_g32(b)),h=(uint32_t)__fp_f32(__fp_g32(a>>32)+__fp_g32(b>>32));return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_sub(uint64_t a,uint64_t b){uint32_t l=(uint32_t)__fp_f32(__fp_g32(a)-__fp_g32(b)),h=(uint32_t)__fp_f32(__fp_g32(a>>32)-__fp_g32(b>>32));return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_mul(uint64_t a,uint64_t b){uint32_t l=(uint32_t)__fp_f32(__fp_g32(a)*__fp_g32(b)),h=(uint32_t)__fp_f32(__fp_g32(a>>32)*__fp_g32(b>>32));return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_div(uint64_t a,uint64_t b){uint32_t l=(uint32_t)__fp_f32(__fp_g32(a)/__fp_g32(b)),h=(uint32_t)__fp_f32(__fp_g32(a>>32)/__fp_g32(b>>32));return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_min(uint64_t a,uint64_t b){uint32_t l=__fp_g32(a)<__fp_g32(b)?(uint32_t)a:(uint32_t)b,h=__fp_g32(a>>32)<__fp_g32(b>>32)?(uint32_t)(a>>32):(uint32_t)(b>>32);return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_max(uint64_t a,uint64_t b){uint32_t l=__fp_g32(a)>__fp_g32(b)?(uint32_t)a:(uint32_t)b,h=__fp_g32(a>>32)>__fp_g32(b>>32)?(uint32_t)(a>>32):(uint32_t)(b>>32);return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_sqrt(uint64_t a){uint32_t l=(uint32_t)__fp_f32(__builtin_sqrtf(__fp_g32(a))),h=(uint32_t)__fp_f32(__builtin_sqrtf(__fp_g32(a>>32)));return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_cvtdq(uint64_t a){uint32_t l=(uint32_t)__fp_f32((float)(int32_t)(uint32_t)a),h=(uint32_t)__fp_f32((float)(int32_t)(uint32_t)(a>>32));return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint32_t __ps_cmp1(float x,float y,int p){int r;switch(p&7){case 0:r=x==y;break;case 1:r=x<y;break;case 2:r=x<=y;break;case 3:r=(x!=x||y!=y);break;case 4:r=!(x==y);break;case 5:r=!(x<y);break;case 6:r=!(x<=y);break;default:r=!(x!=x||y!=y);}return r?0xffffffffu:0u;}\n",
    "static inline uint64_t __ps_cmp(uint64_t a,uint64_t b,uint64_t p){uint32_t l=__ps_cmp1(__fp_g32(a),__fp_g32(b),(int)p),h=__ps_cmp1(__fp_g32(a>>32),__fp_g32(b>>32),(int)p);return (uint64_t)l|((uint64_t)h<<32);}\n",
    "static inline uint64_t __ps_movmsk(uint64_t lo,uint64_t hi){return ((lo>>31)&1)|(((lo>>63)&1)<<1)|(((hi>>31)&1)<<2)|(((hi>>63)&1)<<3);}\n",
    // Wide integer (64-bit 1-operand mul/div via 128-bit), byte swap, bit scan.
    // `__int128` only exists on 64-bit targets; on -m32 (where the transpiler's
    // stack model runs) use bit-exact software equivalents.
    // x86 `div`/`idiv` raise #DE on a zero divisor *or* a quotient that overflows
    // the destination; the hardware does NOT silently truncate. Trap so a
    // transpiled binary crashes deterministically where the original would,
    // never continuing with a wrong value. (`__builtin_trap` is a guaranteed
    // abort with no library dependency; the exact signal is immaterial — we do
    // not model Windows SEH for hardware exceptions anyway.)
    "static inline void __ix_diverr(void){ __builtin_trap(); }\n",
    // 32-bit forms: edx:eax / r/m32. `n` is the 64-bit dividend, `d` the divisor.
    "static inline uint32_t __ix_udiv32(uint64_t n,uint32_t d){ if(!d)__ix_diverr(); uint64_t q=n/d; if(q>0xffffffffull)__ix_diverr(); return (uint32_t)q; }\n",
    "static inline uint32_t __ix_umod32(uint64_t n,uint32_t d){ if(!d)__ix_diverr(); if(n/d>0xffffffffull)__ix_diverr(); return (uint32_t)(n%d); }\n",
    "static inline int32_t __ix_idiv32(uint64_t n,uint32_t d){ if(!d)__ix_diverr(); int32_t dd=(int32_t)d; if(dd==-1&&n==0x8000000000000000ull)__ix_diverr(); int64_t q=(int64_t)n/dd; if(q>2147483647ll||q<-2147483648ll)__ix_diverr(); return (int32_t)q; }\n",
    "static inline int32_t __ix_imod32(uint64_t n,uint32_t d){ if(!d)__ix_diverr(); int32_t dd=(int32_t)d; if(dd==-1&&n==0x8000000000000000ull)__ix_diverr(); int64_t q=(int64_t)n/dd; if(q>2147483647ll||q<-2147483648ll)__ix_diverr(); return (int32_t)((int64_t)n%dd); }\n",
    "#if defined(__SIZEOF_INT128__)\n",
    "typedef unsigned __int128 __u128;typedef __int128 __i128;\n",
    "static inline uint64_t __ix_mul64hi(uint64_t a,uint64_t b){return (uint64_t)(((__u128)a*b)>>64);}\n",
    "static inline uint64_t __ix_imul64hi(uint64_t a,uint64_t b){return (uint64_t)(((__i128)(int64_t)a*(int64_t)b)>>64);}\n",
    "static inline uint64_t __ix_udiv(uint64_t hi,uint64_t lo,uint64_t d){if(!d)__ix_diverr();__u128 q=(((__u128)hi<<64)|lo)/d;if(q>(__u128)0xffffffffffffffffull)__ix_diverr();return (uint64_t)q;}\n",
    "static inline uint64_t __ix_umod(uint64_t hi,uint64_t lo,uint64_t d){if(!d)__ix_diverr();__u128 n=((__u128)hi<<64)|lo;if(n/d>(__u128)0xffffffffffffffffull)__ix_diverr();return (uint64_t)(n%d);}\n",
    "static inline uint64_t __ix_idiv(uint64_t hi,uint64_t lo,uint64_t d){if(!d)__ix_diverr();__i128 n=(__i128)(((__u128)hi<<64)|lo),q=n/(__i128)(int64_t)d;if(q>(__i128)0x7fffffffffffffffll||q<-(__i128)0x8000000000000000ull)__ix_diverr();return (uint64_t)q;}\n",
    "static inline uint64_t __ix_imod(uint64_t hi,uint64_t lo,uint64_t d){if(!d)__ix_diverr();__i128 n=(__i128)(((__u128)hi<<64)|lo),q=n/(__i128)(int64_t)d;if(q>(__i128)0x7fffffffffffffffll||q<-(__i128)0x8000000000000000ull)__ix_diverr();return (uint64_t)(n%(__i128)(int64_t)d);}\n",
    "#else\n",
    "static inline uint64_t __ix_mul64hi(uint64_t a,uint64_t b){uint64_t al=(uint32_t)a,ah=a>>32,bl=(uint32_t)b,bh=b>>32;uint64_t ll=al*bl,lh=al*bh,hl=ah*bl,hh=ah*bh;uint64_t cr=(ll>>32)+(uint32_t)lh+(uint32_t)hl;return hh+(lh>>32)+(hl>>32)+(cr>>32);}\n",
    "static inline uint64_t __ix_imul64hi(uint64_t a,uint64_t b){uint64_t hi=__ix_mul64hi(a,b);if((int64_t)a<0)hi-=b;if((int64_t)b<0)hi-=a;return hi;}\n",
    // NB: the -m32 software 128/64 division below traps on a zero divisor but does
    // not yet detect 64-bit *quotient overflow* (would need `__ix_divmod128` to
    // report a set quotient bit >= 64). Only reachable from a 64-bit-source `div`
    // (absent from 32-bit PEs / the cpudiff corpus); left until 64-bit lifting is
    // exercised so the check ships tested, not guessed.
    "static inline void __ix_divmod128(uint64_t hi,uint64_t lo,uint64_t d,uint64_t*qp,uint64_t*rp){uint64_t q=0,r=0;for(int i=127;i>=0;i--){r=(r<<1)|((i>=64?(hi>>(i-64)):(lo>>i))&1);if(d&&r>=d){r-=d;if(i<64)q|=(uint64_t)1<<i;}}*qp=q;*rp=r;}\n",
    "static inline uint64_t __ix_udiv(uint64_t hi,uint64_t lo,uint64_t d){uint64_t q,r;if(!d)__ix_diverr();__ix_divmod128(hi,lo,d,&q,&r);return q;}\n",
    "static inline uint64_t __ix_umod(uint64_t hi,uint64_t lo,uint64_t d){uint64_t q,r;if(!d)__ix_diverr();__ix_divmod128(hi,lo,d,&q,&r);return r;}\n",
    "static inline uint64_t __ix_idiv(uint64_t hi,uint64_t lo,uint64_t d){if(!d)__ix_diverr();int neg=0;uint64_t nh=hi,nl=lo;if((int64_t)hi<0){neg^=1;nl=~lo+1;nh=~hi+(nl==0);}uint64_t dd=d;if((int64_t)d<0){neg^=1;dd=(uint64_t)(-(int64_t)d);}uint64_t q,r;__ix_divmod128(nh,nl,dd,&q,&r);return neg?(uint64_t)(-(int64_t)q):q;}\n",
    "static inline uint64_t __ix_imod(uint64_t hi,uint64_t lo,uint64_t d){if(!d)__ix_diverr();int neg=(int64_t)hi<0;uint64_t nh=hi,nl=lo;if(neg){nl=~lo+1;nh=~hi+(nl==0);}uint64_t dd=(int64_t)d<0?(uint64_t)(-(int64_t)d):d;uint64_t q,r;__ix_divmod128(nh,nl,dd,&q,&r);return neg?(uint64_t)(-(int64_t)r):r;}\n",
    "#endif\n",
    // x86 parity flag: set iff the low byte of the result has an even number of
    // set bits (`__builtin_parity` returns 1 for odd, so PF is its negation).
    "static inline uint64_t __ix_pf(uint64_t x){return !__builtin_parity((unsigned)(x&0xff));}\n",
    // cpuid: run the *real* host cpuid (a user-mode instruction Wine also lets
    // through), returning the requested result register (0=eax,1=ebx,2=ecx,3=edx)
    // for (leaf, subleaf). The CRT uses it to pick SSE/AVX code paths; the host's
    // features are what the transpiled binary actually runs on, so this is exact.
    "#ifndef __wasm__\n",
    "#include <cpuid.h>\n",
    // We mask off the SSE4.1/4.2 and AVX/AVX2/AVX-512 feature bits (plus the
    // AVX-dependent FMA/F16C and OSXSAVE) so feature dispatchers (notably the CRT's
    // `__isa_available`) pick the SSE2 code paths, which we lift exactly — the
    // SSE4.2 string ops (`pcmpistri`/`pcmpestri`) and the VEX-encoded AVX ops
    // (`vpxor`, `vmovdqu`, …) we do not model. Sound: a CPU with only SSE2 is a
    // valid configuration and the SSE2 paths compute identically (just scalar/
    // 16-byte instead of wider).
    "static inline uint32_t __ix_cpuid(uint32_t leaf,uint32_t sub,uint32_t which){unsigned a=0,b=0,c=0,d=0;__get_cpuid_count(leaf,sub,&a,&b,&c,&d);if(leaf==1)c&=~((1u<<19)|(1u<<20)|(1u<<23)|(1u<<27)|(1u<<28)|(1u<<12)|(1u<<29));if(leaf==7&&sub==0)b&=~((1u<<5)|(1u<<16)|(1u<<17)|(1u<<21)|(1u<<28)|(1u<<30)|(1u<<31));return which==0?a:which==1?b:which==2?c:d;}\n",
    // xgetbv: read the extended control register (XCR0) -> edx:eax. The CRT reads
    // it (after cpuid reports OSXSAVE) to confirm OS-enabled AVX state; the binary
    // only reaches it when the host supports it, so the real instruction is safe.
    // Encoded as raw bytes (0f 01 d0) so it assembles without -mxsave.
    "static inline uint64_t __ix_xgetbv(uint32_t ecx){uint32_t a,d;__asm__ __volatile__(\".byte 0x0f,0x01,0xd0\":\"=a\"(a),\"=d\"(d):\"c\"(ecx));return (uint64_t)a|((uint64_t)d<<32);}\n",
    "#else\n",
    "static inline uint32_t __ix_cpuid(uint32_t leaf,uint32_t sub,uint32_t which){(void)leaf;(void)sub;(void)which;return 0;}\n",
    "static inline uint64_t __ix_xgetbv(uint32_t ecx){(void)ecx;return 3;}\n",
    "#endif\n",
    "static inline uint64_t __ix_bswap32(uint64_t x){return __builtin_bswap32((uint32_t)x);}\n",
    "static inline uint64_t __ix_bswap64(uint64_t x){return __builtin_bswap64(x);}\n",
    "static inline uint64_t __ix_bsf32(uint64_t x){return (uint32_t)x?__builtin_ctz((uint32_t)x):0;}\n",
    "static inline uint64_t __ix_bsr32(uint64_t x){return (uint32_t)x?31-__builtin_clz((uint32_t)x):0;}\n",
    "static inline uint64_t __ix_bsf64(uint64_t x){return x?__builtin_ctzll(x):0;}\n",
    "static inline uint64_t __ix_bsr64(uint64_t x){return x?63-__builtin_clzll(x):0;}\n",
    "static inline uint64_t __ix_tzcnt32(uint64_t x){return (uint32_t)x?__builtin_ctz((uint32_t)x):32;}\n",
    "static inline uint64_t __ix_lzcnt32(uint64_t x){return (uint32_t)x?__builtin_clz((uint32_t)x):32;}\n",
    "static inline uint64_t __ix_tzcnt64(uint64_t x){return x?__builtin_ctzll(x):64;}\n",
    "static inline uint64_t __ix_lzcnt64(uint64_t x){return x?__builtin_clzll(x):64;}\n",
    "static inline uint64_t __ix_popcnt32(uint64_t x){return __builtin_popcount((uint32_t)x);}\n",
    "static inline uint64_t __ix_popcnt64(uint64_t x){return __builtin_popcountll(x);}\n",
    // x87 FPU: 80-bit extended precision == `long double` on x86-64 Linux, so
    // these recompile to equivalent x87 code (bit-exact). Loads widen memory
    // floats/ints to long double; stores narrow back; integer stores truncate
    // (only emitted when the lifter proved truncation rounding or `fisttp`).
    "static inline long double __x87_ld32(uint64_t a){return (long double)*(float*)(uintptr_t)a;}\n",
    "static inline long double __x87_ld64(uint64_t a){return (long double)*(double*)(uintptr_t)a;}\n",
    "static inline long double __x87_ld80(uint64_t a){return *(long double*)(uintptr_t)a;}\n",
    "static inline long double __x87_ild16(uint64_t a){return (long double)*(int16_t*)(uintptr_t)a;}\n",
    "static inline long double __x87_ild32(uint64_t a){return (long double)*(int32_t*)(uintptr_t)a;}\n",
    "static inline long double __x87_ild64(uint64_t a){return (long double)*(int64_t*)(uintptr_t)a;}\n",
    "static inline uint64_t __x87_st32(uint64_t a,long double v){*(float*)(uintptr_t)a=(float)v;return 0;}\n",
    "static inline uint64_t __x87_st64(uint64_t a,long double v){*(double*)(uintptr_t)a=(double)v;return 0;}\n",
    "static inline uint64_t __x87_st80(uint64_t a,long double v){*(long double*)(uintptr_t)a=v;return 0;}\n",
    "static inline uint64_t __x87_ist16(uint64_t a,long double v){*(int16_t*)(uintptr_t)a=(int16_t)(long long)v;return 0;}\n",
    "static inline uint64_t __x87_ist32(uint64_t a,long double v){*(int32_t*)(uintptr_t)a=(int32_t)(long long)v;return 0;}\n",
    "static inline uint64_t __x87_ist64(uint64_t a,long double v){*(int64_t*)(uintptr_t)a=(long long)v;return 0;}\n",
    "static inline long double __x87_add(long double a,long double b){return a+b;}\n",
    "static inline long double __x87_sub(long double a,long double b){return a-b;}\n",
    "static inline long double __x87_mul(long double a,long double b){return a*b;}\n",
    "static inline long double __x87_div(long double a,long double b){return a/b;}\n",
    "static inline long double __x87_abs(long double a){return a<0?-a:a;}\n",
    "static inline long double __x87_neg(long double a){return -a;}\n",
    "static inline long double __x87_sqrt(long double a){return __builtin_sqrtl(a);}\n",
    "static inline long double __x87_rint(long double a){return __builtin_rintl(a);}\n",
    "static inline long double __x87_trunc(long double a){return __builtin_truncl(a);}\n",
    "static inline long double __x87_floor(long double a){return __builtin_floorl(a);}\n",
    "static inline long double __x87_ceil(long double a){return __builtin_ceill(a);}\n",
    "static inline long double __x87_fmod(long double a,long double b){return __builtin_fmodl(a,b);}\n",
    "static inline long double __x87_one(void){return 1.0L;}\n",
    "static inline long double __x87_zero(void){return 0.0L;}\n",
    "static inline uint64_t __x87_lt(long double a,long double b){return a<b;}\n",
    "static inline uint64_t __x87_eq(long double a,long double b){return a==b;}\n",
    "static inline uint64_t __x87_un(long double a,long double b){return a!=a||b!=b;}\n",
    // `fxam`: classify st(0) into the FPU condition codes. C3/C2/C0 encode the
    // IEEE class (NaN=C0, Inf=C2|C0, zero=C3, normal=C2, denormal=C3|C2) and C1
    // is the sign. Bits at hardware positions (C0=8, C1=9, C2=10, C3=14) so a
    // later `fnstsw ax` reads them like the `fcom` idiom does.
    "static inline uint64_t __x87_fxam(long double x){uint64_t s=0;if(__builtin_signbitl(x))s|=(1u<<9);if(__builtin_isnan(x))s|=(1u<<8);else if(__builtin_isinf(x))s|=(1u<<10)|(1u<<8);else if(x==0.0L)s|=(1u<<14);else if(__builtin_isnormal(x))s|=(1u<<10);else s|=(1u<<14)|(1u<<10);return s;}\n",
    // fp return channel: the x87 ABI returns floats in st(0). A fp-returning
    // function stores its st(0) here at `ret`; a caller reads it back after the
    // call. Backed by one shared global (defined in aret_hle.c) so the value
    // crosses translation-unit boundaries between chunks.
    "extern long double __aret_x87_ret;\n",
    "extern int __aret_x87_ret_valid;\n",
    "static inline uint64_t __x87_retstore(long double v){__aret_x87_ret=v;__aret_x87_ret_valid=1;return 0;}\n",
    "static inline long double __x87_retload(void){__aret_x87_ret_valid=0;return __aret_x87_ret;}\n",
    // `rep stos`: fill `n` elements at `d` with the low bytes of `v` (forward, DF=0).
    "static inline uint64_t __rep_stos8(uint64_t d,uint64_t v,uint64_t n){uint8_t* p=(uint8_t*)(uintptr_t)d;for(uint64_t i=0;i<n;i++)p[i]=(uint8_t)v;return 0;}\n",
    "static inline uint64_t __rep_stos16(uint64_t d,uint64_t v,uint64_t n){uint16_t* p=(uint16_t*)(uintptr_t)d;for(uint64_t i=0;i<n;i++)p[i]=(uint16_t)v;return 0;}\n",
    "static inline uint64_t __rep_stos32(uint64_t d,uint64_t v,uint64_t n){uint32_t* p=(uint32_t*)(uintptr_t)d;for(uint64_t i=0;i<n;i++)p[i]=(uint32_t)v;return 0;}\n",
    "static inline uint64_t __rep_stos64(uint64_t d,uint64_t v,uint64_t n){uint64_t* p=(uint64_t*)(uintptr_t)d;for(uint64_t i=0;i<n;i++)p[i]=v;return 0;}\n",
    // rep(ne) scas: scan `n` elements at `p` for `v`, returning the count consumed.
    // `repe` (1) stops on the first mismatch, `repne` (0) on the first match; both
    // stop when the count runs out. (edi/ecx updates and flags are applied by the
    // caller from this count.)
    "static inline uint64_t __rep_scas8(uint64_t d,uint64_t v,uint64_t n,uint64_t repe){const uint8_t* p=(const uint8_t*)(uintptr_t)d;uint8_t x=(uint8_t)v;uint64_t k=0;while(n!=0){int eq=(p[k]==x);k++;n--;if(repe?!eq:eq)break;}return k;}\n",
    "static inline uint64_t __rep_scas16(uint64_t d,uint64_t v,uint64_t n,uint64_t repe){const uint16_t* p=(const uint16_t*)(uintptr_t)d;uint16_t x=(uint16_t)v;uint64_t k=0;while(n!=0){int eq=(p[k]==x);k++;n--;if(repe?!eq:eq)break;}return k;}\n",
    "static inline uint64_t __rep_scas32(uint64_t d,uint64_t v,uint64_t n,uint64_t repe){const uint32_t* p=(const uint32_t*)(uintptr_t)d;uint32_t x=(uint32_t)v;uint64_t k=0;while(n!=0){int eq=(p[k]==x);k++;n--;if(repe?!eq:eq)break;}return k;}\n",
    // ---- Runtime x87 FPU-stack model: the fallback used for functions whose
    // static depth analysis bailed. The stack is ordinary runtime state, so no
    // compile-time depth is needed — correct by construction. Named `__x87rt_`
    // (not `__x87_`) so the optimiser treats them as IMPURE (they mutate the
    // stack): never dropped or reordered. Per-TU state is sound because the x87
    // stack is empty at every call boundary (ABI). `st(0)` = s[p-1].
    // Single SHARED stack (defined in aret_hle.c) — the runtime FPU stack is the
    // real x87 stack: values a runtime-mode callee leaves in st(0) are visible to
    // its runtime-mode caller, exactly as on hardware. So no per-function reset,
    // and cross-chunk calls share it.
    "extern long double __x87rt_s[16];\n",
    "extern int __x87rt_p;\n",
    "extern int __x87rt_rc;\n",
    // Bounds-checked slot access: `st(i)` = s[p-1-i]. An out-of-range access means
    // the runtime stack is inconsistent (e.g. a value an unrecognised callee left
    // in st(0) was never pushed) — TRAP rather than read stale data, keeping the
    // fallback SOUND (correct, or a loud abort; never silently wrong).
    "static inline long double* __x87rt_at(int i){int k=__x87rt_p-1-i;if(k<0||k>=16)__builtin_trap();return &__x87rt_s[k];}\n",
    "static inline void __x87rt_psh(long double v){if(__x87rt_p<0||__x87rt_p>=16)__builtin_trap();__x87rt_s[__x87rt_p++]=v;}\n",
    "static inline uint64_t __x87rt_ld32(uint64_t a){__x87rt_psh((long double)*(float*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ld64(uint64_t a){__x87rt_psh((long double)*(double*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ld80(uint64_t a){__x87rt_psh(*(long double*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ild16(uint64_t a){__x87rt_psh((long double)*(int16_t*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ild32(uint64_t a){__x87rt_psh((long double)*(int32_t*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ild64(uint64_t a){__x87rt_psh((long double)*(int64_t*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ldi(int i){long double v=*__x87rt_at(i);__x87rt_psh(v);return 0;}\n",
    "static inline uint64_t __x87rt_ld1(void){__x87rt_psh(1.0L);return 0;}\n",
    "static inline uint64_t __x87rt_ldz(void){__x87rt_psh(0.0L);return 0;}\n",
    "static inline uint64_t __x87rt_st32(uint64_t a,int pp){*(float*)(uintptr_t)a=(float)*__x87rt_at(0);if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_st64(uint64_t a,int pp){*(double*)(uintptr_t)a=(double)*__x87rt_at(0);if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_st80(uint64_t a,int pp){*(long double*)(uintptr_t)a=*__x87rt_at(0);if(pp)__x87rt_p--;return 0;}\n",
    "static inline long double __x87rt_rnd(long double v){switch(__x87rt_rc){case 1:return __builtin_floorl(v);case 2:return __builtin_ceill(v);case 3:return __builtin_truncl(v);default:return __builtin_rintl(v);}}\n",
    "static inline uint64_t __x87rt_ist16(uint64_t a,int pp){*(int16_t*)(uintptr_t)a=(int16_t)(long long)__x87rt_rnd(*__x87rt_at(0));if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_ist32(uint64_t a,int pp){*(int32_t*)(uintptr_t)a=(int32_t)(long long)__x87rt_rnd(*__x87rt_at(0));if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_ist64(uint64_t a,int pp){*(int64_t*)(uintptr_t)a=(int64_t)__x87rt_rnd(*__x87rt_at(0));if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_sti(int i,int pp){*__x87rt_at(i)=*__x87rt_at(0);if(pp)__x87rt_p--;return 0;}\n",
    "static inline long double __x87rt_op(int o,long double a,long double b){switch(o){case 0:return a+b;case 1:return a-b;case 2:return b-a;case 3:return a*b;case 4:return a/b;default:return b/a;}}\n",
    "static inline uint64_t __x87rt_ar(int o,int ai,int bi,int pp){long double* a=__x87rt_at(ai);long double b=*__x87rt_at(bi);*a=__x87rt_op(o,*a,b);if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_am32(int o,uint64_t a){long double* t=__x87rt_at(0);*t=__x87rt_op(o,*t,(long double)*(float*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_am64(int o,uint64_t a){long double* t=__x87rt_at(0);*t=__x87rt_op(o,*t,(long double)*(double*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ami16(int o,uint64_t a){long double* t=__x87rt_at(0);*t=__x87rt_op(o,*t,(long double)*(int16_t*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_ami32(int o,uint64_t a){long double* t=__x87rt_at(0);*t=__x87rt_op(o,*t,(long double)*(int32_t*)(uintptr_t)a);return 0;}\n",
    "static inline uint64_t __x87rt_xch(int i){long double* a=__x87rt_at(0);long double* b=__x87rt_at(i);long double t=*a;*a=*b;*b=t;return 0;}\n",
    "static inline uint64_t __x87rt_chs(void){long double* t=__x87rt_at(0);*t=-*t;return 0;}\n",
    "static inline uint64_t __x87rt_abs(void){long double* t=__x87rt_at(0);if(*t<0)*t=-*t;return 0;}\n",
    "static inline uint64_t __x87rt_sqrt(void){long double* t=__x87rt_at(0);*t=__builtin_sqrtl(*t);return 0;}\n",
    "static inline uint64_t __x87rt_rndint(void){long double* t=__x87rt_at(0);*t=__x87rt_rnd(*t);return 0;}\n",
    "static inline uint64_t __x87rt_ldcw(uint64_t a){__x87rt_rc=(*(uint16_t*)(uintptr_t)a>>10)&3;return 0;}\n",
    "static inline uint64_t __x87rt_stcw(uint64_t a){*(uint16_t*)(uintptr_t)a=(uint16_t)(0x037F|(__x87rt_rc<<10));return 0;}\n",
    // fp-return handoff for runtime-mode functions (shares the static channel).
    // reset at entry (clean stack), retstore at ret (publish st(0) if fp-returning),
    // pushret after a recognised fp-returning call. Together with the bounds trap
    // above, an unrecognised fp return surfaces as an underflow trap, not garbage.
    "static inline uint64_t __x87rt_reset(void){__x87rt_p=0;return 0;}\n",
    "static inline uint64_t __x87rt_pushret(void){__x87rt_psh(__aret_x87_ret);__aret_x87_ret_valid=0;return 0;}\n",
    // Wrap a call in runtime mode: clear the channel-valid flag before, then push
    // the channel onto the stack after IF a callee wrote it (fp return via channel:
    // static-fp or host libm). A runtime-mode fp callee instead leaves its result
    // on the shared stack (flag stays clear) → no double push. Handles indirect
    // (computed) fp-returning calls the static analysis cannot classify.
    "static inline uint64_t __x87rt_precall(void){__aret_x87_ret_valid=0;return 0;}\n",
    "static inline uint64_t __x87rt_postcall(void){if(__aret_x87_ret_valid){__x87rt_psh(__aret_x87_ret);__aret_x87_ret_valid=0;}return 0;}\n",
    "static inline uint64_t __x87rt_retstore(void){if(__x87rt_p>0)__aret_x87_ret=__x87rt_s[--__x87rt_p];return 0;}\n",
    "static inline uint64_t __x87rt_free(void){if(__x87rt_p>0)__x87rt_p--;return 0;}\n",
    // ---- increment 2: comparisons, status word, transcendentals ----
    "extern unsigned short __x87rt_sw;\n",
    "static inline void __x87rt_setsw(long double a,long double b){int u=(a!=a||b!=b);__x87rt_sw=(unsigned short)(((((a<b)||u)?1:0)<<8)|((u?1:0)<<10)|((((a==b)||u)?1:0)<<14));}\n",
    "static inline uint64_t __x87rt_com(int i,int pp){__x87rt_setsw(*__x87rt_at(0),*__x87rt_at(i));if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_comm32(uint64_t a,int pp){__x87rt_setsw(*__x87rt_at(0),(long double)*(float*)(uintptr_t)a);if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_comm64(uint64_t a,int pp){__x87rt_setsw(*__x87rt_at(0),(long double)*(double*)(uintptr_t)a);if(pp)__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_tst(void){__x87rt_setsw(*__x87rt_at(0),0.0L);return 0;}\n",
    "static inline uint64_t __x87rt_fxam(void){long double x=*__x87rt_at(0);unsigned short s=0;if(__builtin_signbitl(x))s|=(1u<<9);if(__builtin_isnan(x))s|=(1u<<8);else if(__builtin_isinf(x))s|=(1u<<10)|(1u<<8);else if(x==0.0L)s|=(1u<<14);else if(__builtin_isnormal(x))s|=(1u<<10);else s|=(1u<<14)|(1u<<10);__x87rt_sw=s;return 0;}\n",
    "static inline uint64_t __x87rt_getsw(void){return __x87rt_sw;}\n",
    "static inline uint64_t __x87rt_lt(int i){return *__x87rt_at(0)<*__x87rt_at(i);}\n",
    "static inline uint64_t __x87rt_eq(int i){return *__x87rt_at(0)==*__x87rt_at(i);}\n",
    "static inline uint64_t __x87rt_un(int i){long double a=*__x87rt_at(0),b=*__x87rt_at(i);return a!=a||b!=b;}\n",
    "static inline uint64_t __x87rt_cmov(int c,int i){if(c)*__x87rt_at(0)=*__x87rt_at(i);return 0;}\n",
    "static inline uint64_t __x87rt_yl2x(void){long double* y=__x87rt_at(1);*y=*y*__builtin_log2l(*__x87rt_at(0));__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_yl2xp1(void){long double* y=__x87rt_at(1);*y=*y*__builtin_log2l(*__x87rt_at(0)+1.0L);__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_2xm1(void){long double* t=__x87rt_at(0);*t=__builtin_exp2l(*t)-1.0L;return 0;}\n",
    "static inline uint64_t __x87rt_scale(void){long double* t=__x87rt_at(0);*t=__builtin_ldexpl(*t,(int)__builtin_truncl(*__x87rt_at(1)));return 0;}\n",
    "static inline uint64_t __x87rt_sin(void){long double* t=__x87rt_at(0);*t=__builtin_sinl(*t);__x87rt_sw&=~(1u<<10);return 0;}\n",
    "static inline uint64_t __x87rt_cos(void){long double* t=__x87rt_at(0);*t=__builtin_cosl(*t);__x87rt_sw&=~(1u<<10);return 0;}\n",
    "static inline uint64_t __x87rt_sincos(void){long double x=*__x87rt_at(0);*__x87rt_at(0)=__builtin_sinl(x);__x87rt_psh(__builtin_cosl(x));__x87rt_sw&=~(1u<<10);return 0;}\n",
    "static inline uint64_t __x87rt_ptan(void){long double* t=__x87rt_at(0);*t=__builtin_tanl(*t);__x87rt_psh(1.0L);__x87rt_sw&=~(1u<<10);return 0;}\n",
    "static inline uint64_t __x87rt_patan(void){long double* y=__x87rt_at(1);*y=__builtin_atan2l(*y,*__x87rt_at(0));__x87rt_p--;return 0;}\n",
    "static inline uint64_t __x87rt_prem(void){long double* t=__x87rt_at(0);*t=__builtin_fmodl(*t,*__x87rt_at(1));__x87rt_sw&=~(1u<<10);return 0;}\n",
    "static inline uint64_t __x87rt_ldpi(void){__x87rt_psh(3.14159265358979323846L);return 0;}\n",
    "static inline uint64_t __x87rt_ldl2e(void){__x87rt_psh(1.44269504088896340736L);return 0;}\n",
    "static inline uint64_t __x87rt_ldl2t(void){__x87rt_psh(3.32192809488736234787L);return 0;}\n",
    "static inline uint64_t __x87rt_ldlg2(void){__x87rt_psh(0.30102999566398119521L);return 0;}\n",
    "static inline uint64_t __x87rt_ldln2(void){__x87rt_psh(0.69314718055994530942L);return 0;}\n",
);

/// The runtime-helper preamble, included only when the body references it.
pub(crate) fn float_preamble(body: &str) -> &'static str {
    if body.contains("__fp_") || body.contains("__pi_") || body.contains("__ix_") || body.contains("__x87_") || body.contains("__x87rt_") || body.contains("__rep_") {
        FLOAT_HELPERS
    } else {
        ""
    }
}

/// Count instructions that could not be lifted. The lifter represents an
/// unmodelled instruction either as `Stmt::Asm` or as an `asm:`-named call
/// (`asm_fallback`, which also clobbers the written registers to `Undef`). A
/// non-zero count means the decompilation is incomplete and may be incorrect.
pub(crate) fn count_unlifted(func: &IrFunction) -> usize {
    fn is_asm_call(e: &Expr) -> bool {
        matches!(e, Expr::Call { target: CallTarget::Named(n), .. } if n.starts_with("asm:"))
    }
    let mut n = 0;
    for b in &func.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Asm(_) => n += 1,
                Stmt::CallStmt(e) | Stmt::Assign { expr: e, .. } if is_asm_call(e) => n += 1,
                _ => {}
            }
        }
    }
    n
}

pub(crate) fn ctype(bits: u8) -> &'static str {
    match bits {
        8 => "uint8_t",
        16 => "uint16_t",
        32 => "uint32_t",
        _ => "uint64_t",
    }
}

fn ty_ctype(t: &Ty) -> &'static str {
    match t {
        Ty::Int { bits, .. } => ctype(*bits),
        _ => "uint64_t",
    }
}

fn const_c(v: i128) -> String {
    if v < 0 {
        format!("(uint64_t)({})", v)
    } else if v > 9 {
        format!("0x{:x}ULL", v as u128)
    } else {
        format!("{}", v)
    }
}

/// Name a recovered stack-frame slot.
pub(crate) fn frame_name(d: i64) -> String {
    if d == 0 {
        "saved_bp".into()
    } else if d > 0 {
        format!("arg_{:x}", d)
    } else {
        format!("local_{:x}", -d)
    }
}

thread_local! {
    /// The current function's recovered struct layouts (set per function by the
    /// emitter). When a memory address matches a clean struct field, it is
    /// emitted as `((struct S*)base)->field_k` — byte-identical to the raw cast.
    static STRUCT_INFO: std::cell::RefCell<Option<crate::types::StructInfo>> =
        const { std::cell::RefCell::new(None) };
}

/// Set (or clear) the struct layouts consulted by `lvalue_c`.
pub(crate) fn set_struct_info(si: Option<crate::types::StructInfo>) {
    STRUCT_INFO.with(|c| *c.borrow_mut() = si);
}

/// Render a memory lvalue: a recovered struct field access when the base is a
/// struct pointer, else the raw width-typed cast. The two are byte-identical
/// (packed, exact-offset layout), so this never changes semantics.
pub(crate) fn lvalue_c(addr: &Expr, width_bits: u8) -> String {
    let hit = STRUCT_INFO
        .with(|c| c.borrow().as_ref().and_then(|si| si.lookup(addr, (width_bits / 8) as u32)));
    match hit {
        Some((name, base, off)) => format!("((struct {}*)(v{}))->field_{:x}", name, base, off),
        None => format!("(*({}*)({}))", ctype(width_bits), expr_c(addr)),
    }
}

pub(crate) fn expr_c(e: &Expr) -> String {
    match e {
        Expr::Const(v, _) => const_c(*v),
        Expr::Use(v) => format!("v{}", v.0),
        Expr::Undef => "0 /*undef*/".into(),
        Expr::Read(Location::Frame(d)) => frame_name(*d),
        Expr::Read(_) => "0 /*reg*/".into(), // other reads don't remain post-SSA
        Expr::Load { addr, ty } => lvalue_c(addr, int_bits(ty)),
        Expr::Unary(op, x) => match op {
            UnOp::Neg => format!("(-({}))", expr_c(x)),
            UnOp::Not => format!("(~({}))", expr_c(x)),
            // Sign-extension must actually sign-extend: emit a width-aware signed
            // cast (the source width is carried by the masked value or typed
            // load it wraps). Rendering it as identity zero-extends, which is
            // wrong for negative narrow values (e.g. `movsx`).
            UnOp::SignExtend => format!("({})", signed_cast(x)),
            // Zero-extension / truncation: the operand is already masked to its
            // width, so the value is correct as-is.
            _ => format!("({})", expr_c(x)),
        },
        Expr::Binary(op, a, b) => binary_c(*op, a, b),
        Expr::Cast { to, expr } => format!("(({})({}))", ty_ctype(to), expr_c(expr)),
        Expr::Addr(Location::Frame(d)) => format!("(uint64_t)(&{})", frame_name(*d)),
        Expr::Addr(_) => "0 /*addr*/".into(),
        Expr::Call { target, args, .. } => {
            let a: Vec<String> = args.iter().map(expr_c).collect();
            match target {
                CallTarget::Direct(addr) => format!("sub_{:x}({})", addr, a.join(", ")),
                CallTarget::Named(n) if n.starts_with("asm:") => {
                    // An unmodelled instruction in expression position. In the
                    // runnable transpile, fail loud (abort) rather than substitute
                    // 0 for an unknown effect — the comma keeps it an expression.
                    // The read-only decompile keeps the readable comment form.
                    if shared_stack() {
                        format!("(aret_unmodelled({:?}), 0)", n.strip_prefix("asm:").unwrap_or(n))
                    } else {
                        format!("0 /*{}*/", n)
                    }
                }
                CallTarget::Named(n) if shared_stack() && libc_arity(n).is_some() => {
                    // A real libc function (e.g. `memcpy` synthesised from `rep movs`)
                    // called from a transpiled chunk, which has no `<string.h>`
                    // prototype in scope. Our operands are `uint64_t`, so without a
                    // prototype the compiler passes each as a *64-bit* pair on the
                    // 32-bit target — and libc reads its first arg's high word as the
                    // second arg, shifting everything (memcpy then sees src=high(dst),
                    // n=low(src) → a wild copy and crash). Narrow every argument to a
                    // single 32-bit word, which is the correct i386 ABI for these
                    // (pointers/size_t/int are all 32-bit here).
                    let na: Vec<String> = a.iter().map(|x| format!("(uint32_t)({x})")).collect();
                    format!("{}({})", n, na.join(", "))
                }
                CallTarget::Named(n) => format!("{}({})", n, a.join(", ")),
                CallTarget::Indirect(e) => {
                    // Shared-stack/transpile mode: a function pointer holds the
                    // *original* code address, so dispatch through the VA->function
                    // table (`aret_call`) instead of jumping to an unmapped VA.
                    if shared_stack() && !a.is_empty() {
                        format!("aret_call((uint32_t)({}), {})", expr_c(e), a.join(", "))
                    } else {
                        // Call through a function pointer: cast the computed target
                        // to a function-pointer type taking the (over-approximated)
                        // SysV argument registers and returning a 64-bit value.
                        let params = if a.is_empty() {
                            "void".to_string()
                        } else {
                            vec!["uint64_t"; a.len()].join(",")
                        };
                        format!("((uint64_t(*)({}))({}))({})", params, expr_c(e), a.join(", "))
                    }
                }
            }
        }
        Expr::Select { cond, then_, else_ } => {
            format!("({} ? {} : {})", expr_c(cond), expr_c(then_), expr_c(else_))
        }
        Expr::Phi(_) => "0 /*phi*/".into(),
    }
}

pub(crate) fn int_bits(t: &Ty) -> u8 {
    match t {
        Ty::Int { bits, .. } => *bits,
        _ => 64,
    }
}

/// Size of the synthesised per-call stack-frame array, and the entry offset of
/// rsp/rbp within it (leaving room above for stack-passed args / saved slots and
/// below for the function's locals).
const FRAME_SIZE: usize = 16384;
const FRAME_TOP: usize = FRAME_SIZE - 2048;

thread_local! {
    /// Shared machine-stack mode (UBT M3): used by the transpiler so that the
    /// entry rsp/rbp come from a `__esp` parameter (a pointer into one global
    /// stack shared across functions) instead of a private per-call `__frame`
    /// array. This is what lets stack-passed arguments cross function calls. Off
    /// by default, so the verify/decompile paths are unchanged.
    static SHARED_STACK: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// Enable/disable shared machine-stack emission for the current thread.
pub fn set_shared_stack(on: bool) {
    SHARED_STACK.with(|c| c.set(on));
}

pub(crate) fn shared_stack() -> bool {
    SHARED_STACK.with(|c| c.get())
}

/// Render the function's value declarations. Frame-base values (entry rsp/rbp)
/// are pointed at a real per-call `__frame` array so generic stack accesses
/// (arrays, rsp-relative spills) hit real memory instead of dereferencing an
/// uninitialised frame register. Returns the lines (with leading indent).
pub(crate) fn value_decls(values: &BTreeSet<u32>, frame_base: &[u32], fp80: &[u32]) -> String {
    use std::collections::HashSet;
    if values.is_empty() {
        return String::new();
    }
    let fb: HashSet<u32> = frame_base.iter().copied().collect();
    let fp: HashSet<u32> = fp80.iter().copied().collect();
    let used_fb = values.iter().any(|v| fb.contains(v));
    let shared = shared_stack();
    let mut out = String::new();
    if used_fb && !shared {
        // Plain automatic array (per-call, so recursion gets a fresh frame).
        let _ = writeln!(out, "    uint8_t __frame[{}];", FRAME_SIZE);
    }
    // x87 FPU values are 80-bit extended precision → `long double`.
    let ld: Vec<String> = values
        .iter()
        .filter(|v| fp.contains(v))
        .map(|v| format!("v{} = 0", v))
        .collect();
    if !ld.is_empty() {
        let _ = writeln!(out, "    long double {};", ld.join(", "));
    }
    let decls: Vec<String> = values
        .iter()
        .filter(|v| !fp.contains(v))
        .map(|v| {
            if fb.contains(v) {
                // Shared stack: entry rsp/rbp come from the `__esp` parameter (a
                // pointer into the single global stack) so pushed arguments cross
                // calls. Private frame: a fresh per-call array.
                if shared {
                    format!("v{} = __esp", v)
                } else {
                    format!("v{} = (uint64_t)(__frame + {})", v, FRAME_TOP)
                }
            } else {
                format!("v{} = 0", v)
            }
        })
        .collect();
    if !decls.is_empty() {
        let _ = writeln!(out, "    uint64_t {};", decls.join(", "));
    }
    out
}

/// Signed-cast an operand, sign-extending from its apparent width. A value
/// masked to a sub-word width (`x & 0xff/0xffff/0xffffffff`) is sign-extended
/// from that width (`(int64_t)(int32_t)x`), not treated as a positive 64-bit
/// number — otherwise signed comparisons of negative 32-bit values are wrong.
fn signed_cast(e: &Expr) -> String {
    match e {
        // A value masked to a sub-word width is sign-extended from that width.
        Expr::Binary(BinOp::And, x, m) => {
            if let Expr::Const(c, _) = m.as_ref() {
                let st = match *c {
                    0xff => Some("int8_t"),
                    0xffff => Some("int16_t"),
                    0xffffffff => Some("int32_t"),
                    _ => None,
                };
                if let Some(t) = st {
                    return format!("(int64_t)({})({})", t, expr_c(x));
                }
            }
        }
        // A sub-word memory load is read through a signed pointer so its sign bit
        // extends correctly (the default `(*(uintN_t*)…)` would zero-extend).
        Expr::Load { addr, ty } => {
            let b = int_bits(ty);
            if b < 64 {
                return format!("(int64_t)(*({}*)({}))", sctype(b), expr_c(addr));
            }
        }
        _ => {}
    }
    format!("(int64_t)({})", expr_c(e))
}

/// Signed C integer type name for a width in bits.
fn sctype(bits: u8) -> &'static str {
    match bits {
        1..=8 => "int8_t",
        9..=16 => "int16_t",
        17..=32 => "int32_t",
        _ => "int64_t",
    }
}

/// If `e` is sign-extended from a sub-64-bit width — a `& 0xff/0xffff/0xffffffff`
/// mask or a sub-word load — return that width in bits; else `None`. Used to
/// recover the width of a signed comparison so a constant sibling operand can be
/// sign-extended from the *same* width.
fn signed_width(e: &Expr) -> Option<u8> {
    match e {
        Expr::Binary(BinOp::And, _, m) => match m.as_ref() {
            Expr::Const(0xff, _) => Some(8),
            Expr::Const(0xffff, _) => Some(16),
            Expr::Const(0xffffffff, _) => Some(32),
            _ => None,
        },
        Expr::Load { ty, .. } => {
            let b = int_bits(ty);
            if b < 64 {
                Some(b)
            } else {
                None
            }
        }
        _ => None,
    }
}

/// Render `e` truncated to `w` bits for an *equality* comparison: a constant is
/// masked to `w` (so a sign-extended `-1` = `0xffffffffffffffff` becomes the
/// 32-bit `0xffffffff` that the masked operand can actually equal); a non-constant
/// is explicitly `& maskW` so both sides compare their low `w` bits, matching a
/// `cmp r{w}` flag computation. A full-width (`w == 64`) compare is unchanged.
fn mask_w(e: &Expr, w: u8) -> String {
    if !(1..64).contains(&w) {
        return expr_c(e);
    }
    let m: u64 = if w >= 64 { u64::MAX } else { (1u64 << w) - 1 };
    match e {
        Expr::Const(c, _) => const_c(((*c as u128) & (m as u128)) as i128),
        _ => format!("({} & 0x{:x}ULL)", expr_c(e), m),
    }
}

/// Render `e` as a signed `int64_t` interpreted at `w` bits. A constant is
/// sign-extended from `w` (so a 32-bit `0xfff0b9d9` is `-1000999`, not the
/// zero-extended `+4293913049` that silently breaks `cmp r32, imm32; jge`);
/// non-constant operands defer to `signed_cast`, which derives their own width.
fn signed_cast_w(e: &Expr, w: u8) -> String {
    if let Expr::Const(c, _) = e {
        if (1..64).contains(&w) {
            let m = (1u128 << w) - 1;
            let v = (*c as u128) & m;
            let signed: i128 = if (v >> (w - 1)) & 1 == 1 {
                v as i128 - (1i128 << w)
            } else {
                v as i128
            };
            return format!("(int64_t)({})", signed);
        }
    }
    signed_cast(e)
}

fn binary_c(op: BinOp, a: &Expr, b: &Expr) -> String {
    use BinOp::*;
    let u = |x: &Expr| format!("(uint64_t)({})", expr_c(x));
    // Signed comparison/division: interpret both operands at a common width, so a
    // constant operand sign-extends from the same width as its (often masked)
    // sibling rather than being read as a large positive 64-bit value.
    let s = |o: &str| {
        let w = signed_width(a).or_else(|| signed_width(b)).unwrap_or(64);
        format!("({} {} {})", signed_cast_w(a, w), o, signed_cast_w(b, w))
    };
    let un = |o: &str| format!("({} {} {})", u(a), o, u(b));
    let plain = |o: &str| format!("({} {} {})", expr_c(a), o, expr_c(b));
    // Equality at a common width: a `cmp r32, imm` sets ZF from a 32-bit compare,
    // so a sign-extended immediate (-1 → 0xffffffffffffffff) must be truncated to
    // the operand width recovered from a masked sibling — otherwise it never
    // equals the 32-bit operand (0xffffffff), e.g. a `dec; jne -1` loop counter
    // never terminates.
    let eq = |o: &str| {
        let w = signed_width(a).or_else(|| signed_width(b)).unwrap_or(64);
        format!("({} {} {})", mask_w(a, w), o, mask_w(b, w))
    };
    match op {
        Add => plain("+"),
        Sub => plain("-"),
        Mul => plain("*"),
        And => plain("&"),
        Or => plain("|"),
        Xor => plain("^"),
        // Left operand cast to 64-bit: a small/constant operand is otherwise an
        // `int`, and `1 << 32` (shift ≥ width) is undefined behaviour in C.
        Shl => format!("({} << {})", u(a), expr_c(b)),
        Eq => eq("=="),
        Ne => eq("!="),
        Shr => format!("({} >> {})", u(a), expr_c(b)),
        Sar => format!("({} >> {})", signed_cast(a), expr_c(b)),
        UDiv => un("/"),
        UMod => un("%"),
        Ult => un("<"),
        Ule => un("<="),
        Ugt => un(">"),
        Uge => un(">="),
        SDiv => s("/"),
        SMod => s("%"),
        Slt => s("<"),
        Sle => s("<="),
        Sgt => s(">"),
        Sge => s(">="),
    }
}

/// Collect every `ValueId` referenced anywhere (defs, uses, φ args) so they can
/// all be declared (including undef versions that are read but never assigned).
pub(crate) fn collect_values(func: &IrFunction, out: &mut BTreeSet<u32>) {
    fn walk(e: &Expr, out: &mut BTreeSet<u32>) {
        match e {
            Expr::Use(v) => {
                out.insert(v.0);
            }
            Expr::Phi(args) => {
                for v in args {
                    out.insert(v.0);
                }
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Call { target, args, .. } => {
                if let CallTarget::Indirect(x) = target {
                    walk(x, out);
                }
                for a in args {
                    walk(a, out);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Assign { dst, .. } = s {
                out.insert(dst.0);
            }
            match s {
                Stmt::Set { expr, .. }
                | Stmt::Assign { expr, .. }
                | Stmt::CallStmt(expr) => walk(expr, out),
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Branch { cond, .. } => walk(cond, out),
                Stmt::Switch { value, .. } => walk(value, out),
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

/// Collect recovered stack-frame slot displacements (for declaration).
pub(crate) fn collect_frame_vars(func: &IrFunction, out: &mut BTreeSet<i64>) {
    fn walk(e: &Expr, out: &mut BTreeSet<i64>) {
        match e {
            Expr::Read(Location::Frame(d)) | Expr::Addr(Location::Frame(d)) => {
                out.insert(*d);
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Call { target, args, .. } => {
                // An indirect call's target is itself an expression (e.g. a frame
                // slot holding a function pointer) — walk it too, or its frame
                // slots go undeclared.
                if let CallTarget::Indirect(e) = target {
                    walk(e, out);
                }
                for a in args {
                    walk(a, out);
                }
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            if let Stmt::Set { dst: Location::Frame(d), .. } = s {
                out.insert(*d);
            }
            match s {
                Stmt::Set { expr, .. }
                | Stmt::Assign { expr, .. }
                | Stmt::CallStmt(expr) => walk(expr, out),
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Branch { cond, .. } => walk(cond, out),
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

/// Split recovered frame slots into `(params, locals)`. Parameters are the
/// positive slots at/after the first argument (`saved_bp` at 0, return address
/// at one pointer, first argument at two pointers); everything else is a local.
pub(crate) fn frame_params_locals(func: &IrFunction) -> (Vec<i64>, Vec<i64>) {
    let mut fv = BTreeSet::new();
    collect_frame_vars(func, &mut fv);
    let ptr = (func.bits.max(32) / 8) as i64;
    let first_arg = 2 * ptr;
    let mut params = Vec::new();
    let mut locals = Vec::new();
    for d in fv {
        if d >= first_arg {
            params.push(d);
        } else {
            locals.push(d);
        }
    }
    (params, locals)
}

/// The function's C signature. With `with_params`, recovered frame arguments
/// become real parameters; otherwise `(void)`.
pub(crate) fn signature(func: &IrFunction, with_params: bool) -> String {
    // Shared machine-stack mode: every function takes the caller's stack pointer
    // plus the volatile general registers (so both stack- and register-passed
    // arguments cross calls). The register parameters are a fixed list.
    if shared_stack() {
        let mut p = vec!["uint64_t __esp".to_string()];
        for v in &func.reg_params {
            p.push(format!("uint64_t v{}", v));
        }
        return format!("uint64_t sub_{:x}({})", func.entry, p.join(", "));
    }
    if !with_params {
        return format!("uint64_t sub_{:x}(void)", func.entry);
    }
    let mut p: Vec<String> = Vec::new();
    // Register-passed parameters first (64-bit ABIs), in convention order.
    for v in &func.reg_params {
        p.push(format!("uint64_t v{}", v));
    }
    // Then stack/frame parameters.
    let (params, _) = frame_params_locals(func);
    for d in params {
        p.push(format!("uint64_t {}", frame_name(d)));
    }
    if p.is_empty() {
        format!("uint64_t sub_{:x}(void)", func.entry)
    } else {
        format!("uint64_t sub_{:x}({})", func.entry, p.join(", "))
    }
}

/// Declaration line for frame slots that are *not* parameters (or all of them
/// when `with_params` is false).
pub(crate) fn frame_decls(func: &IrFunction, with_params: bool) -> Option<String> {
    let (params, locals) = frame_params_locals(func);
    let set: Vec<i64> = if with_params {
        locals
    } else {
        params.into_iter().chain(locals).collect()
    };
    if set.is_empty() {
        return None;
    }
    let decls: Vec<String> = set.iter().map(|d| format!("{} = 0", frame_name(*d))).collect();
    Some(format!("    uint64_t {};", decls.join(", ")))
}

/// Collect direct call targets, to forward-declare them.
pub(crate) fn collect_callees(func: &IrFunction, out: &mut BTreeSet<u64>) {
    fn walk(e: &Expr, out: &mut BTreeSet<u64>) {
        match e {
            Expr::Call { target: CallTarget::Direct(a), args, .. } => {
                out.insert(*a);
                for x in args {
                    walk(x, out);
                }
            }
            Expr::Call { args, .. } => {
                for x in args {
                    walk(x, out);
                }
            }
            Expr::Load { addr, .. } => walk(addr, out),
            Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => walk(x, out),
            Expr::Binary(_, a, b) => {
                walk(a, out);
                walk(b, out);
            }
            Expr::Select { cond, then_, else_ } => {
                walk(cond, out);
                walk(then_, out);
                walk(else_, out);
            }
            _ => {}
        }
    }
    for b in &func.blocks {
        for s in &b.stmts {
            match s {
                Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => {
                    walk(expr, out)
                }
                Stmt::Store { addr, value, .. } => {
                    walk(addr, out);
                    walk(value, out);
                }
                Stmt::Return(Some(e)) => walk(e, out),
                _ => {}
            }
        }
    }
}

fn stmt_c(s: &Stmt, out: &mut String) {
    match s {
        Stmt::Assign { dst, expr } => {
            let _ = writeln!(out, "    v{} = {};", dst.0, expr_c(expr));
        }
        Stmt::Set { dst: Location::Frame(d), expr } => {
            let _ = writeln!(out, "    {} = {};", frame_name(*d), expr_c(expr));
        }
        Stmt::Set { .. } => {}
        Stmt::Store { addr, value, ty } => {
            let _ = writeln!(out, "    {} = {};", lvalue_c(addr, int_bits(ty)), expr_c(value));
        }
        Stmt::Branch { cond, taken, fallthrough } => {
            let _ = writeln!(out, "    if ({}) goto B{};", expr_c(cond), taken.0);
            let _ = writeln!(out, "    goto B{};", fallthrough.0);
        }
        Stmt::Jump(b) => {
            let _ = writeln!(out, "    goto B{};", b.0);
        }
        Stmt::Switch { value, cases, default } => {
            let _ = writeln!(out, "    switch ({}) {{", expr_c(value));
            for (k, b) in cases {
                let _ = writeln!(out, "        case {}: goto B{};", k, b.0);
            }
            let _ = writeln!(out, "        default: goto B{};", default.0);
            let _ = writeln!(out, "    }}");
        }
        Stmt::Return(Some(e)) => {
            let _ = writeln!(out, "    return {};", expr_c(e));
        }
        Stmt::Return(None) => {
            let _ = writeln!(out, "    return 0;");
        }
        Stmt::CallStmt(e) => {
            let _ = writeln!(out, "    (void)({});", expr_c(e));
        }
        Stmt::Asm(t) => {
            let _ = writeln!(out, "    /* asm: {} */", t);
        }
        Stmt::Nop => {}
    }
}

/// Emit one function as compilable C (goto form). `forward` receives the
/// forward declarations this function needs (callees).
pub fn emit_function(func: &IrFunction, forward: &mut BTreeSet<u64>, with_params: bool) -> String {
    let struct_defs = {
        let si = crate::types::struct_info(func);
        let defs = si.defs();
        set_struct_info(Some(si));
        defs
    };
    let mut f = func.clone();
    destruct_ssa(&mut f);

    let mut values = BTreeSet::new();
    collect_values(&f, &mut values);
    collect_callees(&f, forward);
    if with_params {
        for p in &f.reg_params {
            values.remove(p); // declared as parameters, not locals
        }
    }

    let mut out = String::new();
    out.push_str(&struct_defs);
    let unlifted = count_unlifted(&f);
    if unlifted > 0 {
        let _ = writeln!(
            out,
            "// WARNING: {} unmodelled instruction(s) — decompilation INCOMPLETE, result may be incorrect",
            unlifted
        );
    }
    let _ = writeln!(out, "{} {{", signature(&f, with_params));
    out.push_str(&value_decls(&values, &f.frame_base_values, &f.fp80_values));
    if let Some(fd) = frame_decls(&f, with_params) {
        let _ = writeln!(out, "{}", fd);
    }
    // Enter at the function's entry block.
    let entry_id = f
        .blocks
        .iter()
        .find(|b| b.addr == f.entry)
        .map(|b| b.id)
        .unwrap_or(0);
    let _ = writeln!(out, "    goto B{};", entry_id);
    for b in &f.blocks {
        let _ = writeln!(out, "B{}:;", b.id);
        for s in &b.stmts {
            stmt_c(s, &mut out);
        }
        // A block with no terminator falls through; guard with a return.
        if !ends_in_terminator(b) {
            let _ = writeln!(out, "    return 0;");
        }
    }
    let _ = writeln!(out, "}}");
    set_struct_info(None);
    out
}

/// Make every direct call to a function *defined in this unit* pass exactly the
/// number of arguments that callee's emitted signature declares, so the unit
/// recompiles even though the lifter over-approximates each call to the six
/// SysV integer argument registers.
///
/// Register arguments are kept (truncated, or zero-padded, to the callee's
/// recovered register-parameter count); recovered frame parameters are passed
/// as zero. Calls to a callee *not* defined here are left untouched — their
/// forward declaration uses an empty parameter list (`sub_x();`) and so accepts
/// any arity. This also subsumes recursive self-calls, whose prototype is the
/// function's own definition.
pub(crate) fn fixup_call_arity(funcs: &mut [IrFunction]) {
    use std::collections::HashMap;
    // entry -> (register params, total params incl. frame).
    let arity: HashMap<u64, (usize, usize)> = funcs
        .iter()
        .map(|f| (f.entry, (f.reg_params.len(), param_count(f))))
        .collect();
    for f in funcs.iter_mut() {
        for b in &mut f.blocks {
            for s in &mut b.stmts {
                map_top_exprs(s, &mut |e| fixup_call_expr(e, &arity));
            }
        }
    }
}

fn fixup_call_expr(e: &mut Expr, arity: &std::collections::HashMap<u64, (usize, usize)>) {
    match e {
        Expr::Call { target, args, .. } => {
            for a in args.iter_mut() {
                fixup_call_expr(a, arity);
            }
            match target {
                CallTarget::Indirect(x) => fixup_call_expr(x, arity),
                CallTarget::Direct(addr) => {
                    if let Some(&(nreg, ntot)) = arity.get(addr) {
                        args.truncate(nreg);
                        while args.len() < ntot {
                            args.push(Expr::Const(0, Ty::int(64)));
                        }
                    }
                }
                // A named import has no recovered signature, but well-known libc
                // functions have a fixed arity; trimming the over-approximated
                // argument registers to it removes noise (`strlen(s)` not
                // `strlen(s, …)`). The call has no prototype, so this is safe.
                CallTarget::Named(n) => {
                    if let Some(k) = libc_arity(n) {
                        if args.len() > k {
                            args.truncate(k);
                        }
                    }
                }
            }
        }
        Expr::Load { addr, .. } => fixup_call_expr(addr, arity),
        Expr::Unary(_, x) | Expr::Cast { expr: x, .. } => fixup_call_expr(x, arity),
        Expr::Binary(_, a, b) => {
            fixup_call_expr(a, arity);
            fixup_call_expr(b, arity);
        }
        Expr::Select { cond, then_, else_ } => {
            fixup_call_expr(cond, arity);
            fixup_call_expr(then_, arity);
            fixup_call_expr(else_, arity);
        }
        _ => {}
    }
}

/// Fixed argument count of a common libc function, if known. The name may be a
/// raw import symbol (possibly with a `@plt`/version suffix), so match the stem.
fn libc_arity(name: &str) -> Option<usize> {
    let stem = name
        .split(['@', '+'])
        .next()
        .unwrap_or(name)
        .trim_start_matches("__")
        .trim_end_matches("_chk");
    Some(match stem {
        "strlen" | "free" | "puts" | "fflush" | "fclose" | "perror" | "abort" | "exit"
        | "_exit" | "rewind" | "atoi" | "atol" | "strdup" | "clearerr" | "fileno"
        | "malloc" | "isatty" | "umask" | "close" | "unlink" | "rmdir" | "chdir" => 1,
        "strcmp" | "strcpy" | "strcat" | "fopen" | "fputs" | "fdopen" | "getenv" | "setenv"
        | "rename" | "fseek" | "dup2" | "kill" | "access" | "stat" | "lstat" | "fstat"
        | "strchr" | "strrchr" | "strstr" => 2,
        "memcpy" | "memset" | "memmove" | "strncmp" | "strncpy" | "strncat" | "read"
        | "write" | "memchr" | "fgets" | "lseek" => 3,
        "fread" | "fwrite" => 4,
        _ => return None,
    })
}

/// Apply `f` to each top-level expression of a statement.
fn map_top_exprs(s: &mut Stmt, f: &mut impl FnMut(&mut Expr)) {
    match s {
        Stmt::Set { expr, .. } | Stmt::Assign { expr, .. } | Stmt::CallStmt(expr) => f(expr),
        Stmt::Store { addr, value, .. } => {
            f(addr);
            f(value);
        }
        Stmt::Branch { cond, .. } => f(cond),
        Stmt::Switch { value, .. } => f(value),
        Stmt::Return(Some(e)) => f(e),
        _ => {}
    }
}

/// Number of recovered parameters of `func` (register + frame).
pub(crate) fn param_count(func: &IrFunction) -> usize {
    func.reg_params.len() + frame_params_locals(func).0.len()
}

fn ends_in_terminator(b: &Block) -> bool {
    matches!(
        b.stmts.last(),
        Some(Stmt::Jump(_) | Stmt::Branch { .. } | Stmt::Return(_) | Stmt::Switch { .. })
    )
}

/// Emit a complete compilable C translation unit for the given functions.
pub fn emit_unit(funcs: &[IrFunction]) -> String {
    // Always emit real parameter signatures; an interprocedural fixup then makes
    // every call to a function defined here match its callee's arity, so the
    // unit recompiles (call-site arguments — roadmap §15.4 #2).
    let mut funcs = funcs.to_vec();
    fixup_call_arity(&mut funcs);
    let with_params = true;
    let mut body = String::new();
    let mut forward: BTreeSet<u64> = BTreeSet::new();
    let defined: BTreeSet<u64> = funcs.iter().map(|f| f.entry).collect();
    for f in &funcs {
        body.push_str(&emit_function(f, &mut forward, with_params));
        body.push('\n');
    }
    let mut out = String::new();
    out.push_str("#include <stdint.h>\n\n");
    out.push_str(float_preamble(&body));
    // Forward-declare every function (defined or external) with an empty
    // parameter list, so a call that precedes its callee's definition still has
    // a compatible prototype. The empty list is compatible with the real,
    // parameterised definition emitted below.
    for a in forward.union(&defined) {
        let _ = writeln!(out, "uint64_t sub_{:x}();", a);
    }
    out.push('\n');
    out.push_str(&body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emits_and_lowers_phi() {
        // Diamond with a phi -> after destruction, no Phi remains and copy
        // blocks are inserted.
        let rax = Location::Reg(RegId(0));
        let mut f = IrFunction {
            entry: 0,
            name: "t".into(),
            bits: 64,
            reg_params: vec![],
            frame_base_values: vec![],
            fp80_values: vec![],
            blocks: vec![
                Block { id: 0, addr: 0, stmts: vec![Stmt::Set { dst: rax.clone(), expr: Expr::konst(1, 32) }, Stmt::Branch { cond: Expr::konst(1, 8), taken: BlockId(1), fallthrough: BlockId(2) }], succ: vec![1, 2], pred: vec![] },
                Block { id: 1, addr: 1, stmts: vec![Stmt::Set { dst: rax.clone(), expr: Expr::konst(2, 32) }, Stmt::Jump(BlockId(3)) ], succ: vec![3], pred: vec![0] },
                Block { id: 2, addr: 2, stmts: vec![Stmt::Jump(BlockId(3))], succ: vec![3], pred: vec![0] },
                Block { id: 3, addr: 3, stmts: vec![Stmt::Return(Some(Expr::Read(rax.clone())))], succ: vec![], pred: vec![1, 2] },
            ],
            next_value: 0,
            next_temp: 0,
        };
        crate::ssa::to_ssa(&mut f);
        crate::opt::optimize(&mut f);
        let mut fwd = BTreeSet::new();
        let c = emit_function(&f, &mut fwd, true);
        assert!(c.contains("uint64_t sub_0(void)"));
        assert!(!c.contains("phi"), "phi must be lowered, got:\n{}", c);
        assert!(c.contains("return"));
    }
}
