# module.dll (core)

C++20, MSVC x64. The core runs inside the Roblox client and talks to the desktop
app over a named pipe.

- `src/bootstrap/dllmain.cpp` — attach thread and exports.
- `src/ipc/` — session file and protocol v1 pipe handling.
- `src/rbx/` — scheduler hook, Luau executor, environment/API, drawing, render.
- `src/util/log.cpp` — append-only log at `%LOCALAPPDATA%\zend\logs\core.log`.

Build from the repository root with `.\build.ps1`, or:

```powershell
cmake -S core -B build/core -G "Visual Studio 17 2022" -A x64
cmake --build build/core --config Release
```

The core never patches memory, queries VMAs, opens processes, enumerates
modules, or reads raw HWID values. The loader performs all image work before this
DLL starts. `tools/import-audit.ps1` enforces the forbidden-import list.
