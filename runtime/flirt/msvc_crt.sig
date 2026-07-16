# ARET FLIRT-lite signatures — MSVC statically-linked CRT intrinsics.
# Format: <name> <hex pattern, '..' = wildcard byte> (see mingw_crt.sig).
#
# These recognise hand-assembled MSVC CRT routines whose bodies do NOT lift:
# the classic `memmove`/`memcpy` intrinsic interleaves its alignment jump tables
# with the code (a table entry reads into an adjacent instruction's bytes), which
# no linear/recursive decoder can cleanly separate — so the computed `jmp
# [idx*4+table]` sites are left unmodelled (a sound abort). Binding the whole
# function to the native `aret_memmove`/`aret_memcpy` shim (proven equivalent, see
# below) both fixes the abort and is faithful reuse (the libm precedent).
#
# `memmove` (MSVC, static CRT): prologue loads (dst,src,n) from [ebp+8/0c/10],
# then the overlap check `cmp edi,esi; jbe; cmp edi,esi+n; jb backward` that
# distinguishes memmove from memcpy, then `rep movs` + alignment dispatch. The
# 32-byte prefix below (jcc rel32 displacement wildcarded) is unique to it.
# PROVEN behaviourally equivalent to libc memmove: 500/500 random cases (sizes
# 0..1000, forward/backward overlap) run bit-identically under Unicorn — the
# recognition is measured, not guessed (see docs/vision/71 journal entry).
memmove 558bec57568b750c8b4d108b7d088bc18bd103c63bfe76083bf80f82........
