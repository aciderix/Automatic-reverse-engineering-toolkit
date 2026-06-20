# Finishing the `.UBX` unpack on Windows (OEP + IAT rebuild)

This guide documents the realistic path to a **100% valid, runnable** unpacked
binary for the `.UBX`-protected target, based on everything reverse-engineered
during the Linux emulation effort (see `README.md`).

## Why the Linux/Unicorn approach stops short

The Unicorn emulation reproduces the **entire decompression** faithfully
(validated: real x86 code reappears in `.text`/`.rdata`/`.data`). It then breaks
at the **import-resolution phase** (~3.06 billion instructions in) because the
protector **executes real imported-DLL code** (confirmed: a `user32.dll`
internal routine at the relocated base, doing `mov esi,[user32_IAT]; call esi`).

To run that code, every mapped DLL would itself need its imports linked
recursively and its leaf syscalls emulated — i.e. a full Windows API runtime.
That is what Wine/Windows already is, so the last step belongs there.

Everything *before* that (the metamorphic stub, the decompression algorithm,
the anti-emulation behaviour) is already understood and reproduced.

## Target facts (from the analysis)

- PE32, ImageBase `0x400000`, GUI subsystem, MSVC (links `MSVCR110`).
- Sections `.text`/`.rdata`/`.data` are empty on disk; payload is in `.UBX1`.
- Entry point is inside `.UBX1` (`RVA 0x197b554`), an encrypted metamorphic stub.
- TLS callbacks present (`AddressOfCallBacks` set) — typical anti-debug hook.
- Decompression order at runtime: `.text` → `.rdata` → `.data`, finishing ~2.0B
  emulated instructions; import resolution / IAT build happens ~3.0–3.1B.
- Imports the original program needs (resolved at runtime, not in the on-disk
  table): steam_api, d3d9, d3dx9_43, dsound, dinput8, xinput9_1_0, libcef,
  bink2w32, user32, gdi32, comdlg32, advapi32, shell32, ole32, oleaut32,
  ws2_32, winhttp, version, winmm, kernel32, MSVCR110.
- The on-disk import table (`RVA 0x1979ec0`) is the **packer's** stub imports
  (one function per DLL + `GetModuleHandleA/LoadLibraryA/LocalAlloc/ExitProcess`).

## Recommended Windows workflow

1. **Environment**: a Windows 10 x86 VM (or `wine` on Linux with a 32-bit
   prefix). Put the game DLLs (`GameData/Bin/*.dll`) next to the exe.

2. **Steam dependency**: the stub touches `steam_api` (`SteamUtils` was observed
   being resolved/called). If it gates on Steam, drop in the **Goldberg Steam
   Emulator** `steam_api.dll` (+ a `steam_settings/` with a valid `appid`) so
   `SteamAPI_Init`/`SteamUtils` succeed without a real client. (The gist
   <https://gist.github.com/widberg/daf01e950a7f16836dc6756ddff769a5> is a
   lighter SteamAPI shim that also works for stub-style checks.)

3. **Debugger**: x64dbg (x32dbg) with the **ScyllaHide** plugin enabled
   (defeats the TLS-callback / `IsDebuggerPresent` / timing anti-debug).

4. **Reach the OEP**:
   - Let the stub run. The cleanest OEP catch is a memory breakpoint:
     set `.text` (`0x401000`…) to **no-access** after it has been written, or
     use x64dbg's *"Trace into until"* with condition `eip < 0xdeb14f &&
     eip >= 0x401000`. Execution first enters the original `.text` exactly at
     the OEP.
   - Alternatively use Scylla's *"IAT Autosearch + OEP find"* or the
     `Bin > Trace` heuristics.

5. **Dump + rebuild imports** with **Scylla**:
   - Attach to the process at OEP, *Dump*, then *IAT Autosearch* → *Get Imports*
     → *Fix Dump*. Scylla resolves the IAT against the real loaded DLLs (which is
     the exact step that cannot be done without those DLLs present and linked).

6. **Sanity check**: open the fixed dump in CFF Explorer / PE-bear; the entry
   point should be in `.text`, the import directory should list the real DLLs
   above, and the `.UBX*` sections can be left in place or stripped.

## What you already have

- `snap.exe` (delivered separately): the **fully decompressed image** dumped from
  the Unicorn run — real `.text`/`.rdata`/`.data`. It is not runnable (EP still
  on the stub, game IAT not rebuilt) but is ideal for **static analysis /
  decompilation** (e.g. with ARET, this repo) since the code is recovered.
- The emulation tooling in this folder, which reproduces the decompression and
  can be resumed from a checkpoint to experiment with the final phase.

## If you must stay on Linux

Run the exe under a **working 32-bit Wine prefix** with the game DLLs +
Goldberg `steam_api.dll`, let it self-unpack, then dump the process memory
(`winedump`/`gcore` on the wine process, or a Frida/Unicorn-mode hook at OEP)
and rebuild the IAT the same way Scylla does. This works because Wine supplies
the full, linked DLL environment that pure Unicorn emulation does not.
