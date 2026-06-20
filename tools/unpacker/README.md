# Emulation-based unpacker (Unicorn) — `.UBX` protector case study

Experimental tooling that statically unpacks a PE protected with the Ubisoft
`.UBX` packer by **emulating its unpacking stub** with Unicorn, then dumping the
reconstructed image. Built around the `unipacker` engine (PEB/TEB, fake
kernel32/ntdll, API handlers, OEP detection) with several fixes required by this
protector.

> Target used during development: a `.UBX`-protected 32-bit game binary
> (`MightyQuest.exe`). The binaries, system/game DLLs, memory dumps and
> checkpoints are **not** committed (see `.gitignore`).

## What the protector does (reverse-engineered)

- Original sections (`.text` / `.rdata` / `.data`) are emptied in the file
  (`RawSize = 0`) and compressed into `.UBX1`; the entry point is an **encrypted,
  metamorphic stub** (junk instructions interleaved with the real ones).
- At runtime the stub:
  1. Decompresses `.text` → `.rdata` → `.data` progressively (validated: real
     x86 code reappears). This is heavy: ~2 billion emulated instructions.
  2. Resolves the original program imports by **walking the export tables of the
     real DLLs** it `LoadLibrary`s (steam_api, d3d9, user32, gdi32, …), most
     likely by name-hash. This is why the real DLLs must be present in memory.
  3. Builds the original IAT and tail-jumps to the OEP.

## Key findings / fixes

1. **Self-modifying code** → Unicorn caches translation blocks and runs stale
   bytes unless a *per-instruction* `UC_HOOK_CODE` is present. Block-level or
   range hooks are not enough. This caps throughput at ~1.2M instr/s.
2. `GetModuleHandle`/`LoadLibrary` must return **real module bases** (not
   incrementing fake handles), or the stub walks a bad PE header and crashes.
3. `LocalAlloc`/`LocalFree` must be implemented (returning real allocations),
   otherwise a NULL buffer corrupts the stub ~1.44B instructions in.
4. Import resolution needs the **actual DLLs mapped** with valid export tables.
   System DLLs are taken from Wine's `i386-windows` PE builtins (same export
   names as Windows); game DLLs from the game folder.
5. OEP is detected as the first instruction executed inside the original
   `.text` range; the image is then dumped file-aligned.

## Files

| File | Purpose |
|------|---------|
| `emulate_unpack.py` | Full run: maps real DLLs, emulates the stub, dumps at OEP. |
| `emulate_unpack_checkpoint.py` | Same, but saves a full emulator snapshot at 3.0B instructions. |
| `resume_from_checkpoint.py` | Restores the snapshot and continues in seconds (fast iteration on the final import-resolution / OEP phase). |

## Requirements

```
pip install unicorn capstone pefile unipacker
```

Plus a `dllset/` folder containing the imported DLLs (game DLLs + Wine's
`i386-windows` system DLLs). Paths are currently hard-coded to `/tmp`; adjust as
needed.

## Status

Decompression is **fully reproduced** and the image is recovered (`snap.exe`).
The final blocker is now precisely diagnosed: at ~3.06B instructions the protector
**executes real imported-DLL code** (a `user32.dll` internal routine) which then
calls through `user32`'s own (unresolved) IAT — so finishing the unpack in pure
emulation would require recursively linking every DLL's imports and emulating leaf
syscalls, i.e. reimplementing Wine. The realistic path to a runnable dump is
therefore Windows/Wine-side; see `WINDOWS_UNPACK_GUIDE.md`. The checkpoint/resume
scripts remain useful for experimenting with that final phase in ~1 minute per
iteration instead of ~45.

This is **not** a general-purpose unpacker — it is a documented, reproducible
case study of defeating one protector by emulation, for educational/analysis use
on binaries you are authorized to analyze.
