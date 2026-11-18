# ZEND

Windows x64 Roblox executor. ZEND pairs a Tauri desktop shell with a C++ core
that runs inside the client and a loader proxy that delivers it.

- **Desktop**: Tauri v2, React 19, TypeScript, CodeMirror 6.
- **Core**: `module.dll`, C++20, injected into the Roblox client, talks to the
  app over a named pipe.
- **Loader**: `RobloxPlayerBeta.dll` proxy, ports the reference Hyperion loader
  (payload extraction, image restore, import patch, entry redirect, PEB unlink).
- **Build pin**: every artifact is tied to one client build through
  `builds/ACTIVE` and a per-build manifest.

The repository contains source only. No client binaries, decrypted images, or
payload archives are published.

## Features

### Execution

- One persistent, sandboxed Luau thread per client process. Scripts keep their
  globals, closures, and identity across yields.
- Execution is scheduled through the client's own scheduler step hook; user code
  runs under the client's `xpcall` runner, so runtime errors unwind inside the
  client and return a real traceback with `@Script` line numbers.
- Identity 8 with the full capability mask (`~0`). `setthreadidentity` and
  `getthreadidentity` keep the persistent thread in step.
- Chunks are deferred through `task.defer`; yields, `task.wait`, and nested
  `pcall` work without freezing rendering.
- A dispatch generation guards `__zend_finish`, so a chunk from an old VM cannot
  complete a newer job.
- `print`/`warn` stream to the app while the script is still running, grouped
  under the request id.

### Luau API

| Area | Functions |
|---|---|
| Environment | `getgenv`, `getrenv`, `getreg`, `getgc`, `getinstances`, `getnilinstances`, `getscripts`, `getcallingscript`, `getloadedmodules`, `loadstring`, `_G` |
| Metatables | `getrawmetatable`, `setrawmetatable`, `getmetatable`, `setmetatable`, `setreadonly`, `make_writeable`, `isreadonly` |
| Closures | `iscclosure`, `islclosure`, `checkcaller`, `clonefunction`, `newcclosure`, `isnewcclosure`, `hookfunction`, `hookfunc`, `replaceclosure`, `restorefunction`, `restorefunc`, `isfunctionhooked` |
| Debug | `debug.getinfo`, `debug.getconstants`, `debug.getupvalues`, `debug.setupvalue`, `debug.getproto`, `debug.getstack` (also exposed as globals) |
| Filesystem | `readfile`, `writefile`, `appendfile`, `isfile`, `isfolder`, `listfiles`, `makefolder`, `delfile`, `delfolder` |
| Crypt | `base64encode`, `base64decode`, `base64` table, `crypt.hash` (MD5, SHA-1, SHA-224/256/384/512, SHA-3), `crypt.generatekey`, AES-256-CBC `crypt.encrypt`/`crypt.decrypt` |
| HTTP | `request`, `http_request`, `http_get`, `http_post`, `http` table, `HttpGet`, `HttpPost`, `syn_request`, `httpget`, `httppost` |
| Identity | `getthreadidentity`, `getidentity`, `getthreadcontext`, `setthreadidentity`, `setidentity`, `setthreadcontext`, `getcapabilities` |
| Instances | `getchildren`, `getdescendants`, `findfirstchild`, `findfirstchildofclass`, `findfirstchildwhichisa`, `getpropertychanged` |
| Signals | `getconnections`, `get_signal_cons`, `firesignal`, `getsignalarguments`, `getsignalargumentsinfo`, `hooksignal`, `restoresignal`, `issignalhooked` (plus snake_case aliases) |
| Console | `rconsoleprint`, `consoleprint`, `rconsoleinfo`, `rconsolewarn`, `rconsoleerr`, `rconsoleerror`, `rconsoleclear`, `consoleclear` |
| Touch / prompts | `firetouchinterest`, `fireproximityprompt` |
| Drawing | `Drawing.new` for `Line`, `Text`, `Circle`, `Square`, `Triangle`, `Quad` |

Details worth knowing:

- **Filesystem** is sandboxed under `%LOCALAPPDATA%\zend\workspace`. Paths are
  normalized and rejected when they escape the root; executable and script
  extensions cannot be written.
- **HTTP** runs on a WinHTTP worker and polls with `task.wait`, so slow requests
  yield instead of blocking the client scheduler. Responses expose `Success`,
  `StatusCode`, `StatusMessage`, `Headers`, `Cookies`, and `Body`; the reference
  privacy blocklist is enforced.
- **Closure hooks** preserve the target's identity: Lua closures get a
  dispatcher proto, C closures a dispatcher `c.f` with a continuation. Hooks may
  yield. `hookfunction(target, original)` uninstalls.
- **Signals** use the reference sentinel walk to find connections. `hooksignal`
  installs a Luau wrapper into each live connection slot, passes
  `Info = {Connection, Function, Index}`, and lets the hook return `false` to
  stop the connection.
- **Drawing** is rendered in-game by an ImGui D3D11 layer hooked at `Present`
  with `ResizeBuffers` handling, so resize and fullscreen do not break it.
  `Image` is intentionally unsupported.

### Desktop app

Five views: scripts, hub, autoexec, console, settings.

- CodeMirror 6 editor with Luau highlighting, multiple tabs, wrap, and
  unsaved-change confirmation.
- `Ctrl+Enter` / `F5` run, `Ctrl+S` save, `Ctrl+W` close, `Ctrl+Tab` cycle,
  `Ctrl+L` clear console.
- Autoexec runs once per client pid after the configured delay.
- Hub accepts the built-in manifest, inline JSON, or a manifest URL, caches
  remote sources, and can open or run an entry.
- Console filters by level, copies visible lines, and groups streamed output
  under a per-exec header.
- Settings cover wrap, clear-on-run, pipe override, inject delay, loader
  channel, accent theme, always-on-top, and console filters. Settings, session,
  autoexec, and hub cache persist in `localStorage`.
- Frameless window with Attach/Eject, pin (always-on-top), and a global
  `Ctrl+Shift+Z` show/hide hotkey.
- The pinned client build is embedded at compile time; a session that reports a
  different build shows a warning badge in the title bar and status bar.

### Updates

ZEND checks an update feed and never installs anything silently. Settings →
Updates shows the current version and a **Check now** button. When the feed
reports a newer version, the panel shows the release notes and a **Download**
button that opens the URL in the default browser.

The feed is a small JSON document:

```json
{
  "version": "0.1.1",
  "url": "https://github.com/0xLLVEL/zend/releases/download/v0.1.1/ZEND_0.1.1_x64-setup.exe",
  "published": "2026-11-20",
  "notes": "Short release notes shown in the panel."
}
```

- The default feed is `updates.json` on `main`
  (`https://raw.githubusercontent.com/0xLLVEL/zend/main/updates.json`).
- The feed field accepts an HTTPS URL or a local file path, which is useful for
  testing a release before publishing it.
- Versions are compared as `major.minor.patch`; pre-release suffixes are
  ignored, so a beta with the same core version does not prompt.
- The app only reads the feed and opens the link. Downloading and running the
  installer stay manual.

### Loader

- Embedded `payload.7z` with a per-build manifest.
- Validates the live exe header (`SizeOfImage` and entry point) before touching
  memory.
- Restores `.text` and `.pdata`, applies import patches, redirects the entry
  point, maps `module.dll`, waits for initialization, then unlinks the module
  from the PEB.
- Fail-open: a header mismatch or any failure **before** the image is restored
  loads `RobloxPlayerBeta.stock.dll` from the same version directory inside
  `DllMain` and forwards `run`. Failures after restore are fatal by design.

### Tooling

| Tool | Purpose |
|---|---|
| `tools/offsetdump` | Build `build.json`, `loader.offsets.json`, and `core.offsets.json` from a decrypted client image; validate imports, entry thunk, `.pdata`, core offsets, payload hash, and optionally the live exe. |
| `tools/repack` | Rebuild `payload.bin` from `.text` + unwind data and produce a deterministic `payload.7z`; asserts size, SHA-256, entry size, and CRC. |
| `tools/detlog` | Read the loader's `DET1` shared section or `detlog.jsonl`; print events, heat, and a verdict summary. QA builds only. |
| `tools/soak` | Sample `status` and DET1 heat into JSONL with queue/heat/disconnect budgets. |
| `tools/import-audit.ps1` | Fail the build when `module.dll` imports a forbidden API. |

## Architecture

```text
┌─────────────────────────┐         named pipe v1         ┌───────────────────────────┐
│ ZEND.exe                │ ◄───────────────────────────► │ module.dll (core)         │
│ Tauri + React + Rust    │   session-<pid>.json, token   │ scheduler hook, Luau, API │
└─────────────────────────┘                               └─────────────▲─────────────┘
                                                                        │ mapped
┌──────────────────────────────────────────────┐                        │
│ RobloxPlayerBeta.dll (loader proxy)          │────────────────────────┘
│ payload 7z, restore, imports, entry, PEB     │
└──────────────────────────────────────────────┘
```

The core writes `%LOCALAPPDATA%\zend\runtime\session-<pid>.json` with the pipe
name, token, pid, and build. The desktop supervisor watches that directory,
reconnects to the newest session, and falls back to a manual attach. Protocol v1
frames are a 4-byte little-endian length followed by JSON; the first frame must
be `hello` with the session token, and frames are capped at 16 MiB.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 Build Tools with the C++ x64 workload
- CMake 3.25+ (the VS-bundled CMake works)
- Node 22+ and npm
- Rust stable with the MSVC toolchain
- WebView2 runtime (the installer embeds the bootstrapper)

## Build

Native artifacts (core, loader, test host):

```powershell
.\build.ps1
```

Outputs:

```text
build/core/Release/module.dll
build/loader/Release/RobloxPlayerBeta.dll
build/tests/Release/load-run.exe
build/tests/Release/RobloxPlayerBeta.stock.dll
```

The loader reads `builds/ACTIVE`, then the matching `build.json`, and embeds the
per-build payload archive generated by `tools/repack`. If no payload is present,
CMake builds a fail-open-only proxy instead of failing. The loader refuses to
configure when `config.h` names a different build or when the manifest payload
size disagrees with the compiled constants.

Desktop:

```powershell
cd apps\desktop
npm ci
npm run check      # tsc --noEmit
npm run build      # frontend bundle
npm run tauri dev  # desktop window
```

Installer:

```powershell
cd apps\desktop
npm run tauri build
```

The NSIS bundle lands in
`apps\desktop\src-tauri\target\release\bundle\nsis\`. It installs per user under
`%LOCALAPPDATA%\Programs\ZEND`, embeds the WebView2 bootstrapper, and leaves
runtime data in `%LOCALAPPDATA%\zend`. Uninstall removes the program, shortcuts,
and registry entries but keeps user data.

## Deployment

1. Close Roblox.
2. In the client version directory, keep the original DLL as
   `RobloxPlayerBeta.stock.dll`.
3. Replace `RobloxPlayerBeta.dll` with `build/loader/Release/RobloxPlayerBeta.dll`.
4. Copy `build/core/Release/module.dll` next to it.
5. Launch Roblox normally, then open ZEND and attach.

ZEND never launches Roblox itself. Replacing DLLs while the client is running is
not supported, and the loader must match the client build exactly; otherwise it
hands off to the stock DLL so the client still boots.

## Updating for a new client build

1. Generate a new manifest with `tools/offsetdump` from the new client image and
   dumper output.
2. Rebuild the payload with `tools/repack` and verify its size and hash against
   the manifest.
3. Write the new directory name to `builds/ACTIVE`.
4. Rebuild the loader and desktop app. The desktop binary embeds the new build id
   and flags any session that reports a different one.

`build.json` carries the image size, entry point, `.text`/`.pdata` geometry, all
900 import slots, core offsets, payload size/SHA-256, and the live client exe
hash/size. `loader.offsets.json` and `core.offsets.json` hold the full per-build
tables.

## Tests

Hermetic checks (no Roblox):

```powershell
node tests\mock-core.mjs        # terminal 1: protocol v1 over a named pipe
node tests\pipe-check.mjs       # terminal 2: hello, ping, status, exec
node tests\soak-check.mjs       # soak harness against the mock core
python tests\detlog-selftest.py # DET1 ring reader, including wrap-around
.\build.ps1
pwsh tests\fail-open.ps1        # fake stock DLL, expects run() == 42
pwsh tests\core-smoke.ps1       # real module.dll and real pipe
pwsh tools\import-audit.ps1     # forbidden-import audit
```

Live checks (client must be running):

```text
tests/f3-check.mjs              exec + print + errors
tests/f4-check.mjs              yields, task.wait, tracebacks
tests/f5-env-check.mjs          environment helpers
tests/f6-meta-debug.mjs         metatables, closures, debug
tests/f7-filesystem-crypt.mjs   filesystem sandbox + crypt
tests/f8-hooks.mjs              closure hooks
tests/f9-http.mjs               HTTP with a local server
tests/f10-identity-instance.mjs identity, instances, loaded modules
tests/f11-signals.mjs           getconnections / firesignal
tests/f12-signals-hook.mjs      signal arguments + hooksignal
tests/f13-rconsole.mjs          rconsole output
tests/f14-touch-proximity.mjs   touch + proximity prompts
tests/stream-check.mjs          streamed print/warn output
```

Long-run soak:

```powershell
node tools\soak\soak.mjs --hours 24
```

Heat is read from the loader's `DET1` section through `tools/detlog` and is only
available on a loader built with `-DZEND_DETLOG=ON`. Shipped builds keep
`ZEND_DETLOG=OFF`; the soak run then checks the pipe path only.

## Repository layout

```text
apps/desktop/     Tauri v2 + React 19 + CodeMirror 6 shell
core/             module.dll (C++20, CMake)
loader/           RobloxPlayerBeta.dll proxy (C++20, CMake)
shared/protocol/  wire contract (JSON Schema, TypeScript, C++)
builds/           pinned build id, per-build manifests
updates.json      public update feed read by the app
tools/            offsetdump, repack, detlog, soak, import audit
tests/            mock core, native harness, Luau regression suites
build.ps1         builds core + loader + test host
```

## Design constraints

- The core never patches memory, queries VMAs, opens processes, enumerates
  modules, or reads raw HWID values. The loader owns all image and import work.
  `tools/import-audit.ps1` enforces the forbidden-import list.
- Fail-open is limited to the window before `restore()` writes the image. After
  that point, a failure is fatal; silently falling back would leave a half-patched
  client.
- Loader and core share one manifest. A mismatch refuses the attach.
- `repack` is deterministic: timestamps and attributes are stripped and the same
  input produces byte-identical `payload.bin` and `payload.7z`.
- The core uses a static CRT (`/MT`); JSON goes through vendored picojson.
- Detection logging is a QA switch, not a shipping surface.

## Known limitations

- Build-locked: a Roblox update requires a fresh manifest and payload before the
  loader will run.
- `Drawing.Image` is not implemented.
- `hooksignal` does not wrap connections created after the hook, and a second
  hook on the same signal is ignored.
- `:Wait()` signal connections are not exposed (`Thread` is nil), and a
  connection disabled through the wrapper is not unlinked by a later native
  `Disconnect`.
- rconsole implements print/info/warn/error/clear, not the full reference set.
- One chunk executes at a time on the persistent thread; the queue reports depth
  but scripts are serialized.
- Windows x64 only.

## Third-party

- Luau (MIT)
- Dear ImGui (MIT)
- LZMA SDK (public domain)
- 7-Zip CLI/DLL (LGPL)
- picojson (BSD-2-Clause)
- Tauri, React, CodeMirror (MIT)

## Disclaimer

This project is for research and development on Windows clients you own. Using
an executor can violate Roblox's Terms of Service and lead to account
suspension. There is no warranty; use it at your own risk.
