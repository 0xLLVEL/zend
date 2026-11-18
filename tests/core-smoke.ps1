# F2 core smoke: load module.dll into a plain host, then run the protocol check
# against that host's session file. exec is expected to fail until F3.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$module = Join-Path $root "build\core\Release\module.dll"
$hostExe = Join-Path $root "build\tests\Release\load-run.exe"
$session = Join-Path $env:LOCALAPPDATA "zend\runtime\session-"

foreach ($path in @($module, $hostExe)) {
    if (!(Test-Path $path)) { throw "missing $path (run .\build.ps1)" }
}

$proc = Start-Process -FilePath $hostExe -ArgumentList @("`"$module`"", "-", "6") -PassThru -NoNewWindow
$sessionFile = "$session$($proc.Id).json"

try {
    $deadline = (Get-Date).AddSeconds(5)
    while (!(Test-Path $sessionFile) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (!(Test-Path $sessionFile)) {
        throw "core did not write $sessionFile"
    }

    & node (Join-Path $root "tests\pipe-check.mjs") --pid $proc.Id --allow-no-exec
    if ($LASTEXITCODE -ne 0) { throw "pipe-check failed" }

    $proc.WaitForExit()
    if (!(Test-Path $sessionFile)) {
        "core-smoke: session file removed on unload"
    } else {
        "core-smoke: warning - stale session file left behind"
        Remove-Item $sessionFile -Force -ErrorAction SilentlyContinue
    }
} finally {
    if (!$proc.HasExited) {
        $proc.Kill()
        $proc.WaitForExit()
        Remove-Item $sessionFile -Force -ErrorAction SilentlyContinue
    }
}

"core-smoke: pass"
