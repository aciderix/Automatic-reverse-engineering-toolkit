# Gauntlet corpus — grandeur-nature measurement pass

21 varied real-world Win32 (i686 PE) binaries used for the *grandeur-nature*
regression: each is transpiled by ARET and its native output compared
bit-for-bit against the Wine reference in a single pass.

Run with:

```bash
bench/gauntlet/score.sh
```

The binaries live compressed in `gauntlet-bins.tar.gz` (committed so they
survive a container reset) and are auto-extracted to `bench/gauntlet/bins/`
on first run. `gauntlet-bins.tar.gz` sha256:
`db9d74445cebcef903527ebb1883397f52d15401d330af4eaa14adc15ba4604d`.

## Provenance

Most were cross-compiled from upstream source with the MinGW-w64 i686
toolchain (`i686-w64-mingw32-gcc`), typically:

```bash
./configure --host=i686-w64-mingw32 CFLAGS="-O2 -g0" && make -j4
```

`build.sh` (committed here) automates that for the GNU-style tarballs.
`*_stripped.exe` are `i686-w64-mingw32-strip`-ed copies of their symboled
sibling — kept to exercise stripped-binary function recovery.

| Binary                     | Source                                             | Toolchain / notes |
|----------------------------|----------------------------------------------------|-------------------|
| bzip2.exe / _stripped      | bzip2 1.0.8 (sourceware.org)                        | mingw |
| grep.exe / _stripped       | GNU grep 3.11 (ftp.gnu.org)                         | mingw |
| gzip.exe / _stripped       | GNU gzip 1.13 (ftp.gnu.org)                         | mingw |
| hello.exe / _stripped      | GNU hello 2.12.1 (ftp.gnu.org)                      | mingw |
| lua.exe                    | Lua 5.4.7 (lua.org)                                 | mingw |
| m4.exe / _stripped         | GNU m4 1.4.19 (ftp.gnu.org)                         | mingw |
| minigzip.exe / _stripped   | zlib 1.3.1 test tool (zlib.net)                     | mingw |
| nasm.exe                   | NASM 2.16.01 win32 (nasm.us) — **MSVC-built**       | prebuilt upstream, stripped |
| sed.exe                    | GNU sed 4.9 (ftp.gnu.org)                           | mingw (+ `-lbcrypt`) |
| sqlite3.exe                | SQLite 3.40.1 CLI (sqlite.org) — **MSVC-built**     | prebuilt upstream |
| sqlite3_stripped.exe       | sqlite3.exe stripped                                | mingw strip |
| sqlite3_full.exe / _stripped | SQLite 3.40.1 amalgamation, `shell.c sqlite3.c`   | mingw, `-DSQLITE_THREADSAFE=0` |
| units.exe / _stripped      | GNU units 2.23 (ftp.gnu.org)                        | mingw |

MSVC-built binaries (nasm, sqlite3) are the interesting hard cases: no
MinGW CRT, different codegen, and (nasm) already stripped upstream.

## Per-binary sha256 (extracted `.exe`)

```
1e6d245fe31c01d12cfdd2adeaf0bcbd4642c6c18d712b9f69a5fa443f79606a  bzip2.exe
02a069280e7302f965377972db6f926cb78faa72a47edf1ea78fd4cf72861470  bzip2_stripped.exe
a9af1a7e8e875b641a874ca6ce8ca53f3eb1eaabcec484bbf0cafca58cc878a9  grep.exe
649e7dc27a5e21291379b7da0240c8deaf86b9f5c865b9805064e2d891013dfb  grep_stripped.exe
e274936516d141f64b1a9cc4bd7f0254bcfdbda131cfee86d9b10aa5250dd37e  gzip.exe
c2e120df6ae362348dd2cbcb3ae5fee7e09d51a4bdf5504ba24b97870b5c324f  gzip_stripped.exe
80201465872b92f1d7579735362db99c42d13f77bda78bc18ad0ab7dc3ee01c2  hello.exe
07e6c2687a69a925373684f358d9bbe7addd3c8f21e61c86f37c9881fb4afc35  hello_stripped.exe
ba99c87f2b5e0cbebb343d4a4bd897c66c6eba35824a79460cf08578f5b3083c  lua.exe
fd91704e390d755ef334466613c70692417ffe42745e415f968194b16ba86e59  m4.exe
09286c15b05fafb00631c8e092bbae0c80ec16c18f91e97610f1acc4615a84be  m4_stripped.exe
2c8c784a935ceb964c23fbcb42de026753c4cc51f47638b20afc3fd2f201e321  minigzip.exe
76d774d9b7dcb3539fab2da72cd756fc8d743b7039dd05fbe62083c5495fdae9  minigzip_stripped.exe
8a2c7c5f15aebe636b3444acedb575074160afd0e7994baa559b83a7a2a19284  nasm.exe
f67847bebff9b80a37666e5182b78d926a7e9e41103bdba76968b000ebefca29  sed.exe
5c26ea8bdf2c3d4dab69ba784d3ea95bce94cfcf4711688ce6fa0aae1712761c  sqlite3.exe
10b9391c1ac59e8700b4c55d3575fa6f29f1c555648f3b7dd2425f201dd52935  sqlite3_full.exe
ed0109f72592fa842246820f4d03421fdc1c2246e647c35f566ddd95d07cf7ff  sqlite3_full_stripped.exe
512cda74465718cba9acefae8698de6cac8b3231d10fb530f565c2b48f55d1cc  sqlite3_stripped.exe
b7bd3173361808e1c54cf75ee175a7fca9d80c544eadf2c4e7a83ca7581c79cc  units.exe
a875a71fc8bf8a3ee1899aaf6ce6414e459d070b6b1820e70577e7841dd21f15  units_stripped.exe
```

## License note

These are unmodified builds of upstream free software (GPL/BSD/MIT/zlib as
per each project) kept purely as reverse-engineering test fixtures. Sources
are recoverable from the URLs above; `build.sh` reproduces the mingw builds.
