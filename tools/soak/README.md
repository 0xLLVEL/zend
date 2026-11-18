# soak

Samples a live client for the soak budget. Keeps one pipe connection open,
sends `status` every interval, and appends one JSON object per sample.

```powershell
node tools\soak\soak.mjs --hours 24
node tools\soak\soak.mjs --pid 23592 --minutes 10
node tools\soak\soak.mjs --samples 3 --interval 0.1 --no-heat   # check the harness
```

Output defaults to `%LOCALAPPDATA%\zend\logs\soak-<utc>.jsonl`. Each line:

```json
{"iso":"...","ts":1794978314105,"pid":23592,"build":"4ea8e85481e34f46",
 "ready":true,"hooked":true,"queue":0,"place_id":0,
 "heat":4,"events":12,"drop":0}
```

## Budget

| Flag | Default | Fails when |
|---|---|---|
| `--max-heat` | 60 | `heat` above the limit |
| `--max-queue` | 4 | `queue` above the limit |
| `--max-disconnects` | 10 | more pipe drops than allowed |
| `--connect-timeout` | 60 s | no live session is found |
| `--fail-fast` | off | exit on the first failure instead of at the end |

Exit codes: `0` clean, `1` budget or session failure, `2` bad usage / no
session.

`ready`/`hooked` are read straight from `status.result`. Any false value is a
failure; a missing field (mock core) is treated the same way, so both the real
core and the mock advertise them explicitly.

## Heat

`heat` is read from the loader's `DET1` section through
`tools/detlog/detlog.py`. It exists only on a QA build:

```powershell
cmake -S loader -B build/loader-detlog -G "Visual Studio 17 2022" -A x64 -DZEND_DETLOG=ON
cmake --build build/loader-detlog --config Release
```

Deploy that `RobloxPlayerBeta.dll` while Roblox is closed. Shipped builds keep
`ZEND_DETLOG=OFF`, so `heat` stays `null` and the soak run continues; only the
pipe checks are active.

For a verdict breakdown rather than the aggregate heat, run
`python tools\detlog\detlog.py --pid <pid> -n 500 --summary` after a session.
