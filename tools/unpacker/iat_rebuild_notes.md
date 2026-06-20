# Rebuilding OEP + IAT from the Wine run (Scylla-style, on Linux)

Once the binary self-unpacks under Wine (`wine_unpack.py`), the unpacked code and
a populated import table live in the running process. Turning that into a
standalone, runnable PE is exactly what **Scylla** does on Windows. Notes and a
partial implementation for doing it from Linux via `/proc`:

## Stabilising the process (solved)

The game exits ~1–2 s after the OEP because its telemetry/online init fails to
resolve Ubisoft hosts. Point those hosts at a non-routable IP so the socket
`connect()` blocks and the process **hangs well past the OEP** with the IAT fully
built:

```
# /etc/hosts
10.255.255.1 hqbloomberg.ubisoft.org
10.255.255.1 onlineconfigservice.ubi.com
10.255.255.1 popauthenticationservice.ubi.com
10.255.255.1 public-ubiservices.ubi.com
```

(Adding the **Goldberg Steam Emulator** `steam_api.dll` makes the Steam side
succeed too, which can be needed depending on the launch path.)

Then `SIGSTOP` the 32-bit pid that has `0x00400000` mapped and read
`/proc/<pid>/maps` (DLL load bases) and `/proc/<pid>/mem` (the unpacked image).

## IAT resolution (framework + caveats)

1. From `/proc/<pid>/maps`, get each Wine/game DLL's in-process base.
2. From each DLL **file**, build `rva -> export name`. Function address in the
   process = `in_process_base + export_rva`.
3. Find the game IAT: scan `.text` for `FF15`/`FF25` (`call/jmp [imm32]`) to
   collect import-thunk slots, read their values, resolve to `(dll, name)`.
4. Emit a fresh import directory + IAT and patch it into the dumped PE.

**Caveats that need care (this is why a battle-tested tool like Scylla is
preferable):**
- Wine resolves many exports as **forwarders** (the `AddressOfFunctions` entry
  points into the export section as a string) and may forward differently than
  Windows — addresses must be followed through forwarders.
- Under wow64 the DLL load base is not always the lowest `maps` segment; verify
  by reading the `MZ`/`PE` header at the candidate base.
- The freeze must happen **after** the IAT is populated (post-OEP), not at first
  `.text` decompression — use the hang trick above so timing is not racy.

## Recommended finish

Run the real **Scylla** (or x64dbg + Scylla) attached to the Wine process (Scylla
runs fine under Wine) or on Windows: *IAT Autosearch → Get Imports → Fix Dump*.
It handles forwarders/redirections and OEP search robustly. Everything needed
(the binary self-unpacks, the DLL environment, a way to keep the process alive)
is now in place.

## Finding: the IAT is obfuscated (on-demand decryption)

A live snapshot of the import table region (start of `.rdata`, RVA ~`0x9ec000`)
shows the real imports **scattered among high-entropy encrypted values**, e.g.
only the functions already called are present as valid pointers
(`kernel32!GetTickCount`, `ntdll!RtlEnterCriticalSection`,
`advapi32!RegQueryValueExA`, …) while the surrounding slots hold garbage.

This means the protector **encrypts the IAT and decrypts entries on demand** (or
routes calls through per-import decrypt stubs). Consequences:

- A single memory dump never contains the full clean IAT — only the imports
  exercised so far are decrypted.
- A plain Scylla *Get Imports* is insufficient; defeating this needs either
  forcing every import path to execute (so every entry decrypts) or reversing
  the per-import decryption stub. That is a separate, protector-specific effort.

So: decompression + execution are fully solved (the binary self-unpacks and runs
under Wine), but a 100%-valid standalone rebuild is gated on the IAT obfuscation,
which is an advanced protection beyond a one-shot dump.

## Result: headless import harvesting via the binding routine

A write-watchpoint on the IAT region (resumed from the 3.0B checkpoint) shows the
protector binds imports with a single generic routine — `mov [eax], edx` at a
fixed address (eax = IAT slot, edx = resolved address). Capturing every write and
resolving `edx` against the mapped DLL export tables recovers the imports
**headless, without running the game** (`harvest_imports_emulated.py`): 55 imports
across kernel32/ntdll/user32/advapi32 were recovered this way up to the point
emulation diverges.

Coverage limit: the binary binds imports **lazily** — only the entries needed up
to a given execution point are written; the rest resolve on demand as the game
exercises features. So full coverage needs either faithfully emulating the called
DLL functions (i.e. a Wine-class environment) or **unioning several Wine runs**
that trigger different features. There is no single trigger that binds everything,
because the binary only binds what it actually uses.

## OEP recovered: RVA 0x6545e0

The original entry point was found statically (no single-stepping) via the CRT
signature: `__security_init_cookie` (RVA 0x6676d0, calls QueryPerformanceCounter
+ GetTickCount) → its caller `__tmainCRTStartup` (RVA 0x667750: chkstk + SEH +
/GS cookie + GetStartupInfo) → the wrapper calling it with **no internal callers**
= the entry point at **RVA 0x6545e0**. Setting `AddressOfEntryPoint = 0x6545e0`
on the decompressed image yields a cleanly-loadable unpacked PE for analysis
(`find_oep.py`).
