# bench/eh — MSVC C++ exception-handling fixtures

The winecorpus fixtures are built with **mingw** (GCC), which does **not** emit the MSVC
C++ EH model (`_CxxThrowException` / `__CxxFrameHandler3` + FuncInfo/TryBlock/CatchableType
tables). Real 1990s–2000s Windows C++ apps (MFC, Delphi, MSVC) use exactly that model, so
to develop and prove ARET's C++ EH against a ground truth we build the fixtures with
**clang targeting the MSVC ABI** instead.

## How it works

`build.sh SRC.cpp OUT.exe`:

1. `clang --target=i686-pc-windows-msvc` compiles the C++ to a COFF object that references
   the real MSVC EH primitives (`__CxxThrowException@8`, `___CxxFrameHandler3`, type
   descriptors).
2. `gen_msvcrt_lib.py` parses the export table of **Wine's** `msvcrt.dll` → a `.def`;
   `llvm-dlltool` turns it into an i386 import library. So the fixture imports the EH
   runtime from `msvcrt` at load time — resolved by **Wine** (the oracle) and by **ARET's
   HLE** (under test) alike.
3. `eh_support.c` bridges two gaps: the stdcall-decorated `__CxxThrowException@8` →
   the undecorated msvcrt export, and a dummy `type_info` vftable (throw/catch matching
   compares mangled type *names*, never calling through the vtable).
4. `lld-link` produces the PE (entry `mainCRTStartup`, no CRT init, `msvcrt` only).

The **same PE** runs under Wine (`wine OUT.exe`) and under ARET
(`aret OUT.exe --mode transpile --run`) → C++ EH is measured **bit-for-bit vs Wine**.

## Handler versions (measured)

- **clang (modern)** emits `__CxxFrameHandler3` (FuncInfo magic `0x19930522`).
- **Real 1990s binaries** (e.g. WinZip's `WZ32.DLL` = 66 EH regions, `WZSEPE32.EXE`) use
  the original `__CxxFrameHandler` (magic `0x19930520`). The FuncInfo layout differs
  (v3 adds `EHFlags` + `pESTypeList`), so ARET keys its handler on the magic and supports
  both: clang fixtures exercise v3, the real corpus exercises v1, both under the Wine
  oracle.

## Requirements

`clang`, `lld-link`, `llvm-dlltool`, `python3`, and Wine 32-bit (for `msvcrt.dll` + oracle).
