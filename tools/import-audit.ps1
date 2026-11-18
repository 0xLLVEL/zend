# Core import audit. Fails when module.dll grows a forbidden import.
#
# The static CRT drags in a small set of Kernel32 imports for unwind/abort
# handling. Those are allowlisted below and are not called from core sources;
# a source grep guards that claim.

param(
    [string]$Binary = "build/core/Release/module.dll",
    [string]$Source = "core/src"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (![System.IO.Path]::IsPathRooted($Binary)) { $Binary = Join-Path $root $Binary }
if (![System.IO.Path]::IsPathRooted($Source)) { $Source = Join-Path $root $Source }

if (!(Test-Path $Binary)) { throw "missing $Binary (build core first)" }

$forbidden = @(
    "VirtualQuery",
    "VirtualProtect",
    "NtProtectVirtualMemory",
    "NtQueryVirtualMemory",
    "OpenProcess",
    "NtQuerySystemInformation",
    "CreateToolhelp32Snapshot",
    "EnumProcessModules",
    "Module32First",
    "Module32Next",
    "EnumThreadWindows",
    "RtlLookupFunctionEntry",
    "GetSystemFirmwareTable",
    "GetAdaptersAddresses"
)

# Kernel32 imports that the statically linked CRT needs for unwind and abort.
# They are dormant unless the core throws or aborts.
$crtAllow = @(
    "VirtualProtect",
    "RtlLookupFunctionEntry"
)

function Find-Dumpbin {
    $onPath = Get-Command dumpbin.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if ($onPath) { return $onPath }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find "VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe" 2>$null |
            Select-Object -First 1
        if ($found -and (Test-Path $found)) { return $found }
    }

    $roots = @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio")
    foreach ($rootDir in $roots) {
        if (!(Test-Path $rootDir)) { continue }
        $found = Get-ChildItem $rootDir -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like "*Hostx64\x64*" } |
            Select-Object -First 1 -ExpandProperty FullName
        if ($found) { return $found }
    }
    return $null
}

$dumpbin = Find-Dumpbin
if (!$dumpbin) { throw "dumpbin.exe not found (Visual Studio x64 tools)" }

$imports = (& $dumpbin /nologo /imports $Binary | Out-String)
$hits = @()
foreach ($name in $forbidden) {
    if ($imports -match "(?m)^\s+[0-9A-F]+\s+$([regex]::Escape($name))\s*$") {
        $hits += $name
    }
}

$bad = @($hits | Where-Object { $crtAllow -notcontains $_ })
if ($bad.Count -gt 0) {
    throw "forbidden imports in ${Binary}: $($bad -join ', ')"
}

$sourcePatterns = @()
foreach ($name in $forbidden) {
    # "OpenProcess(" must not match "OpenProcessToken(".
    $sourcePatterns += "\b$([regex]::Escape($name))\s*\("
}

$sourceHits = Get-ChildItem $Source -Recurse -Include *.cpp,*.hpp |
    Select-String -Pattern $sourcePatterns
if ($sourceHits) {
    $lines = $sourceHits | ForEach-Object { "$($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
    throw "forbidden API in core sources:`n$($lines -join "`n")"
}

# Unlike the loader, the core must never contain the module path or its own
# filename: the loader owns discovery and loading.
$bytes = [System.IO.File]::ReadAllBytes($Binary)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
foreach ($needle in @("module.dll", "RobloxPlayerBeta.dll")) {
    if ($text.Contains($needle)) { throw "core binary contains '$needle'" }
}

"import-audit: pass (allowlisted CRT internals: $($crtAllow -join ', '))"
