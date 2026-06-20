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
