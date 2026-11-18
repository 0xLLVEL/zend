param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"

$cmake = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) {
    $cmake = "cmake"
}

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

foreach ($name in @("core", "loader", "tests")) {
    $src = Join-Path $root $name
    $out = Join-Path $build $name

    & $cmake -S $src -B $out -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    & $cmake --build $out --config Release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Get-ChildItem -Path $build -Recurse -Include module.dll, RobloxPlayerBeta.dll |
    Select-Object FullName, Length |
    Format-Table -AutoSize
