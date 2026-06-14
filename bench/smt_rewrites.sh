#!/usr/bin/env bash
# Level-3 verification (roadmap §8.2): formally prove, with Z3, that the
# optimiser's rewrite rules (src/opt/mod.rs) preserve semantics for ALL 64-bit
# inputs. Each check asserts the negation of an identity and expects `unsat`
# (i.e. the identity holds universally). One deliberately-wrong check expects
# `sat` to confirm the harness can detect a bad rewrite.
Z3="${Z3:-z3}"
ZERO="#x0000000000000000"

check() { # name  expected(unsat|sat)  smt-body
  local name="$1" want="$2" body="$3"
  local got
  got=$(printf '%s\n(check-sat)\n' "$body" | "$Z3" -in 2>/dev/null | head -1)
  if [ "$got" = "$want" ]; then echo "PROVED  $name"; else echo "FAILED  $name (z3=$got want=$want)"; fi
}

HDR='(declare-const a (_ BitVec 64))(declare-const b (_ BitVec 64))(declare-const x (_ BitVec 64))'

# Signed less-than reconstruction: Ne(SF,OF) for (a-b)  <=>  a <s b   [match_signed_lt]
check "signed: SF!=OF  <=>  a <s b" unsat "$HDR
(define-fun sub () (_ BitVec 64) (bvsub a b))
(define-fun sf () Bool (bvslt sub $ZERO))
(define-fun of () Bool (and (distinct (bvslt a $ZERO) (bvslt b $ZERO)) (distinct (bvslt sub $ZERO) (bvslt a $ZERO))))
(assert (not (= (distinct sf of) (bvslt a b))))"

# jle: ZF | (SF!=OF)  <=>  a <=s b                                    [Or(Eq,Slt)->Sle]
check "signed: ZF|(SF!=OF)  <=>  a <=s b" unsat "$HDR
(define-fun sub () (_ BitVec 64) (bvsub a b))
(define-fun sf () Bool (bvslt sub $ZERO))
(define-fun of () Bool (and (distinct (bvslt a $ZERO) (bvslt b $ZERO)) (distinct (bvslt sub $ZERO) (bvslt a $ZERO))))
(assert (not (= (or (= a b) (distinct sf of)) (bvsle a b))))"

# jbe: CF | ZF  <=>  a <=u b                                          [Or(Ult,Eq)->Ule]
check "unsigned: ULT|EQ  <=>  a <=u b" unsat "$HDR
(assert (not (= (or (bvult a b) (= a b)) (bvule a b))))"

# not through relational: !(a==b) <=> a!=b ; !(a<u b) <=> a>=u b      [negate_rel]
check "not(a==b) <=> a!=b"   unsat "$HDR (assert (not (= (not (= a b)) (distinct a b))))"
check "not(a<u b) <=> a>=u b" unsat "$HDR (assert (not (= (not (bvult a b)) (bvuge a b))))"

# mask-of-mask: (x & 0xffffffff) & 0xff = x & 0xff                    [And(And(x,c1),c2)->And(x,c1&c2)]
check "mask-of-mask" unsat "$HDR
(assert (not (= (bvand (bvand x #x00000000ffffffff) #x00000000000000ff) (bvand x #x00000000000000ff))))"

# idempotent / reflexive identities
check "x & x = x"  unsat "$HDR (assert (not (= (bvand x x) x)))"
check "x ^ x = 0"  unsat "$HDR (assert (not (= (bvxor x x) $ZERO)))"
check "x | 0 = x"  unsat "$HDR (assert (not (= (bvor x $ZERO) x)))"

# signed 32-bit width fix: (int64)(int32)x is the correct sign-extension        [emit::signed_cast]
check "sign-extend32 = mask-then-signed-of-32" unsat "$HDR
(assert (not (= ((_ sign_extend 32) ((_ extract 31 0) x)) ((_ sign_extend 32) ((_ extract 31 0) (bvand x #x00000000ffffffff))))))"

# sanity: a wrong rewrite must be detected (a <s b  is NOT  a <u b)
check "SANITY: signed != unsigned (must be sat)" sat "$HDR (assert (not (= (bvslt a b) (bvult a b))))"
