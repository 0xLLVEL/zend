# Desktop shell

Tauri v2 + React 19 + TypeScript + CodeMirror 6.

```powershell
npm install
npm run dev        # browser preview only
npm run check      # tsc --noEmit
npm run build      # frontend bundle
npm run tauri dev  # desktop window
```

## Structure

```
src/
  components/    titlebar, rail, script list, workbench, editor, console,
                 autoexec, hub, settings, status
  data/          scripts, settings/session, hub manifest stores
  styles/        app.css (tokens + layout)
src-tauri/       Rust shell, pipe client, window config, hotkey thread
```

## Current behavior

- The Rust backend watches `%LOCALAPPDATA%\zend\runtime`, attaches to the newest
  session file, and reconnects automatically when a core appears. A pipe-name
  override in Settings filters the supervisor and explicit attach.
- Attach state, build, pid, queue depth, and 5 s heartbeat latency come from the
  core, not from hardcoded UI state. Eject asks the core to detach.
- Five views: scripts (multi-tab editor + console), hub, autoexec, console, and
  settings. View, active script, settings, autoexec source, and hub cache survive
  a reload via `localStorage`.
- Run sends `exec`; `result` output and errors land in the console. Streamed
  `output` events are deduped by request id and grouped under a per-exec header.
- `log` and `toast` events from the core are rendered as they arrive.
- Ctrl+Enter / F5 run (CodeMirror binding, no stray newline), Ctrl+S saves the
  active doc, Ctrl+W closes it with an unsaved-changes confirm, and Ctrl+Tab
  cycles tabs. Ctrl+L clears the console. `Ctrl+Shift+Z` toggles the window
  globally through a raw `user32` hotkey thread.
- Autoexec runs once per client pid after the inject delay; it also has a manual
  Run now action. Hub supports the built-in manifest, inline JSON, and URL fetch,
  with a local cache for remote scripts.
- Settings cover wrap, clear-on-run, pipe override, inject delay, loader channel,
  accent theme (ember/mint/azure), topmost, and console level filters.
- Window is frameless; minimize/maximize/close, drag region, pin, and close are
  wired. Always-on-top uses the Tauri window API.

## Packaging

```powershell
npm run tauri build
```

The NSIS installer is written to
`src-tauri\target\release\bundle\nsis\`. It installs per user under
`%LOCALAPPDATA%\Programs\ZEND` and embeds the WebView2 bootstrapper.
