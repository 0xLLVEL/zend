# detlog

Reads the loader's detection ring. There are two sources, both produced by the
loader when `kDebugLog` is enabled:

| Source | Path | Notes |
|---|---|---|
| shared section | `Local\RobloxDetLog_<pid>` | live, 4096-event ring, overwritten in place |
| JSON lines | `<logdir>\detlog.jsonl` (usually `%TEMP%`) | append-only, survives a crash |

The reader only maps the section with `FILE_MAP_READ`; running it against a
live client is safe.

## Usage

```powershell
python tools\detlog\detlog.py --list          # client processes with a section
python tools\detlog\detlog.py --pid 9620 -n 60
python tools\detlog\detlog.py --pid 9620 --follow
python tools\detlog\detlog.py --pid 9620 --json --summary
python tools\detlog\detlog.py --log $env:TEMP\detlog.jsonl -n 200
```

Without `--pid` the tool looks for a running `RobloxPlayerBeta.exe` that
exposes the section, then falls back to `%TEMP%\detlog.jsonl`.

`--json` prints one object per event. Addresses are hex strings, matching the
`detlog.jsonl` lines written by the loader. `rva` is relative to the client
image base, so it can be compared with the build manifest directly.

## Layout

Header (48 bytes):

```
u32 magic 'DET1'  u32 version  u32 capacity  u32 head
u64 imageBase     u64 startTick
u32 heat          u32 drop     u32 pid       u32 pad
```

Event (64 bytes):

```
u64 tick (QPC)  u64 caller  u64 a0  u64 a1  u64 a2  u64 a3
u32 tid  u16 kind  u8 source  u8 verdict  u32 seq  u32 pad
```

`heat` decays toward 0 over 30 s and saturates at 100. Only `Suspicious` (4)
and `Confirmed` (20) events raise it; `Observed` events are normal client
traffic (timing probes alone fire thousands of times per minute), so they are
logged to the ring without moving the gauge. `drop` counts events suppressed by
the 1 s repeat filter. `seq` is monotonic; the newest event is `seq == head - 1`.

The offsets are duplicated in `tests/detlog-selftest.py`, which writes a
synthetic ring and runs the reader over it. If `loader/include/emulator/detlog.h`
changes, that test fails together with the `static_assert`s.

## Enabling

Configure the loader with `-DZEND_DETLOG=ON` (sets `kDebugLog`) for a QA build.
Keep it `OFF` for shipped builds: the section and the log file are observable
from other processes.

The JSONL mirror is opt-in: set `ZEND_DETLOG_FILE=1` in the client's
environment to append `detlog.jsonl` next to the module (or in `%TEMP%`).
Without it only the shared section is written — timing probes alone can append
hundreds of megabytes per session, and the soak reads the section.
