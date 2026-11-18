# Tests

## Protocol

- `mock-core.mjs` — protocol v1 core over a named pipe. Writes the session
  file, fakes `print` output, simulates `error(...)`, answers
  `ping`/`status`, and removes the session file on exit. Never touches Roblox.
- `pipe-check.mjs` — talks to the newest session (`--pid <pid>` to pin one).
  Asserts `hello`, `pong`, `status.result`, then one `exec`.
  `--allow-no-exec` accepts a core that has not wired execution yet.

```powershell
node tests\mock-core.mjs        # terminal 1
node tests\pipe-check.mjs       # terminal 2
```

- `soak-check.mjs` — starts `mock-core.mjs`, runs `tools/soak` for three
  samples, and checks the JSONL output and summary. Hermetic.
- `detlog-selftest.py` — creates a synthetic `DET1` ring (including wrap-around)
  and reads it back through `tools/detlog`. Hermetic.

```powershell
node tests\soak-check.mjs
python tests\detlog-selftest.py
```

## Native

- `load-run.cpp` — test host: `load-run <dll> [export|-] [seconds]`. Loads a
  DLL, optionally calls one export, and keeps it loaded so another process can
  use the pipe.
- `fake-stock.cpp` — fake `RobloxPlayerBeta.stock.dll` whose `run()` returns 42.
- `fail-open.ps1` — builds a sandbox with the proxy and the fake stock, then
  asserts the proxy returns 42 from `run()` and logs the handoff.
- `core-smoke.ps1` — loads the real `module.dll` into `load-run.exe`, waits for
  `session-<pid>.json`, runs `pipe-check.mjs --allow-no-exec`, and asserts the
  session file is removed on unload.

```powershell
.\build.ps1
pwsh tests\fail-open.ps1
pwsh tests\core-smoke.ps1
```

These never launch Roblox.

Live suites (`f3`–`f14`, `stream-check`) need a running client; see the root
README for the full list and commands.
