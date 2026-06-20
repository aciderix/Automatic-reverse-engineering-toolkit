# Snapshot tools — the A+B "save-state" pipe

Run a program once to a stable, initialized state (A), capture its memory, then
seed that state into ARET-lifted functions so they run headless from a CLI (B).

## A — capture (`dump_snapshot.py`)

Freeze the target process at a stable point, then dump its mapped memory:

```sh
# Example: a game self-unpacked under Wine, hung past the OEP (online init
# blocked via /etc/hosts), then SIGSTOP'd:
python3 dump_snapshot.py <pid> game.snap            # low 2 GiB by default
python3 dump_snapshot.py <pid> game.snap 0x400000 0x1e00000
```

Output `game.snap` is `ARETSNP1` then records `va:u64-LE len:u64-LE bytes` —
the readable regions in the window. This is where the protector's on-demand IAT
is already resolved and globals are initialized.

## B — run lifted code against it (ARET `--snapshot`)

```sh
aret --mode transpile --entry 0x<func_va> --snapshot game.snap \
     --run --iat-symbols iat_symbols_full.json MightyQuest_unpacked.exe
```

`--snapshot` seeds the transpiled program's initial memory from the capture
(instead of the static sections), so the lifted `sub_<func_va>` sees the real
post-init globals/heap. Build a small driver that calls it with chosen inputs to
drive the game's logic from the command line — no GUI, no Direct3D.

## Caveats

- Snapshot at a **stable** point; if the lifted function mutates global state,
  re-seed per call (deterministic) or accept drift.
- Imports the lifted code calls go through ARET's HLE shims (network/etc. can be
  stubbed), not the snapshot's resolved addresses.
