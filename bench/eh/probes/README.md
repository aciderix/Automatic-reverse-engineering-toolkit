# `bench/eh/probes` — measurement harnesses, deliberately NOT gates

`bench/ehdiff.sh` globs `bench/eh/*.c` and builds each one through the clang
MSVC-ABI path. Everything here needs the **mingw** toolchain and a `.def` instead,
because it calls a CRT entry point directly rather than letting a compiler emit
`__try`. Left one directory up, these turn the EH gate red for a reason nobody
should fix — and doc 70 section 7 is explicit that an unstable gate is worse than a
slow one, because it teaches people to ignore red.

These are the harnesses that established the `_except_handler4_common` contract
(doc 81 I4, instrument-first). They are kept so the next session measures instead of
rebuilding:

| probe | question it answered |
|---|---|
| `eh4_probe.c` | is the frame's scope-table pointer XOR-encoded with `*cookie`? **yes**, proven both ways |
| `eh4_probe_layout.c` | four-int header then records; walk order; **terminator is -2, not -1**; `gs/eh_cookie_offset = -2` means absent and `check_cookie` is never called |
| `eh4_probe_trylevel.c` | is `trylevel` encoded too? **no**, stored plain — proven both ways with a cookie of 4 and eight levels, so neither answer can crash |
| `eh4_probe_xpointers.c` | filter ebp is `frame+16` and `EXCEPTION_POINTERS` goes to `[frame-4]`; the v4 frame's own `xpointers` field at +20 is left **untouched** |

Build and run any of them:

```sh
cd bench/eh/probes
i686-w64-mingw32-dlltool -d eh4_probe.def -l eh4.a
i686-w64-mingw32-gcc -O0 -w eh4_probe_layout.c eh4.a -o p.exe
i686-w64-mingw32-objdump -p p.exe | grep _except_handler4_common   # MUST show it
wine p.exe
```

The `objdump` line is not optional: mingw supplies its own bodies for several CRT
entry points, and a probe that resolves locally measures the compiler rather than
the CRT.
