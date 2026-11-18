# F2 fail-open: the proxy must hand off to RobloxPlayerBeta.stock.dll when the
# live exe header does not match the pinned build. The fake stock returns 42.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$proxy = Join-Path $root "build\loader\Release\RobloxPlayerBeta.dll"
$fake = Join-Path $root "build\tests\Release\RobloxPlayerBeta.stock.dll"
$hostExe = Join-Path $root "build\tests\Release\load-run.exe"
$sandbox = Join-Path $root "build\tests\failopen"

foreach ($path in @($proxy, $fake, $hostExe)) {
    if (!(Test-Path $path)) { throw "missing $path (run .\build.ps1)" }
}

New-Item -ItemType Directory -Force $sandbox | Out-Null
Copy-Item $proxy (Join-Path $sandbox "RobloxPlayerBeta.dll") -Force
Copy-Item $fake (Join-Path $sandbox "RobloxPlayerBeta.stock.dll") -Force

& $hostExe (Join-Path $sandbox "RobloxPlayerBeta.dll") "run" "0"
$code = $LASTEXITCODE
if ($code -ne 42) {
    throw "fail-open returned $code, expected 42"
}

$log = Join-Path $env:LOCALAPPDATA "zend\logs\loader.log"
if (Test-Path $log) {
    $tail = Get-Content $log -Tail 40
    if (!($tail -match "fail-open: stock ready")) {
        throw "loader.log does not show the fail-open handoff"
    }
}

"fail-open: pass (stock run returned 42)"
