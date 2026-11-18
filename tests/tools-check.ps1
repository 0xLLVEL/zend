$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$dump = Join-Path $root "dump"

& python (Join-Path $root "tests\detlog-selftest.py")
if ($LASTEXITCODE -ne 0) { throw "detlog selftest failed" }
$exe = Join-Path $dump "RobloxPlayerBeta_4ea8e85481e34f46_decrypted.exe"
$profile = Join-Path $dump "hyperion.hpp"
$offsets = Join-Path $dump "offsets.hpp"
$clientDump = Join-Path $dump "hyperion_win_live_client_version-4ea8e85481e34f46.txt"

foreach ($path in @($exe, $profile, $offsets, $clientDump)) {
    if (-not (Test-Path $path)) {
        Write-Host "tools-check: SKIP (missing $path)"
        exit 0
    }
}

$out = Join-Path $root "build\tools-check"
$repackOut = Join-Path $out "payload.bin"
$archive = Join-Path $out "payload.7z"
New-Item -ItemType Directory -Force $out | Out-Null

& python (Join-Path $root "tools\offsetdump\offsetdump.py") `
    --exe $exe --profile $profile --offsets $offsets `
    --client-dump $clientDump --out $out
if ($LASTEXITCODE -ne 0) { throw "offsetdump failed" }

$build = Get-Content (Join-Path $out "build.json") -Raw | ConvertFrom-Json
$expect = @{
    imageSize = 155983872
    epOrig = 155975684
    epNew = 100450076
    textRva = 4096
    textSize = 107046876
    runtime = 150794240
    rtCount = 277330
    lua = 70073344
    idJson = 49347600
    imports = 900
}
foreach ($name in $expect.Keys) {
    $actual = $build.loader.$name
    if ($actual -ne $expect[$name]) {
        throw "build.loader.$name = $actual, expected $($expect[$name])"
    }
}
if ($build.payload.size -ne 110374844) {
    throw "payload size $($build.payload.size), expected 110374844"
}
if ($build.payload.sha256 -ne "2d27024c6fa1cba76cf0487f2509be988aff07e7e106641b36c795a7dc0619df") {
    throw "unexpected payload hash $($build.payload.sha256)"
}
if ($build.core.fireTouchInterest -ne "0x14D0000") {
    throw "unexpected FireTouchInterest $($build.core.fireTouchInterest)"
}

& python (Join-Path $root "tools\repack\repack.py") `
    --exe $exe --manifest (Join-Path $out "build.json") `
    --out $repackOut --archive $archive
if ($LASTEXITCODE -ne 0) { throw "repack failed" }

$archiveBytes = (Get-Item $archive).Length
if ($archiveBytes -lt 20000000) {
    throw "archive looks truncated ($archiveBytes bytes)"
}

Write-Host "tools-check: PASS (payload $($build.payload.size), archive $archiveBytes)"
