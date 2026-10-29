#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Fails when ddb.dll, ddbcmd.exe or gdal-util.exe depends on a GPL library (Exiv2, bm3d) or on Qt,
    or when bm3d.exe depends on Qt.

.DESCRIPTION
    Windows counterpart of scripts/check-gpl-linkage.py. Exiv2 is used only by the separate
    metadata-util.exe, and bm3d (with the Nexus, VCGLib and corto code it is derived from) only by
    the separate bm3d.exe, a static program with no DLL of its own; libddb starts both as child
    processes. A build whose ddb.dll or ddbcmd.exe imports exiv2.dll, a bm3d*.dll or a Qt5*.dll
    would link that code into the library. Qt is the other way round: bm3d.exe decodes textures
    with plain image codec libraries and must not import a Qt DLL, which would bring back the Qt
    DLLs and plugins the package no longer carries. gdal-util.exe (MPL-2.0) embeds libddb the way
    ddbcmd.exe does, so it is held to the rule of ddb.dll and ddbcmd.exe. The symbol check (and
    its positive control) of the Linux script has no counterpart here: dumpbin only lists
    imports.

    The imports are read with `dumpbin /dependents`, which is found on PATH (a Visual Studio
    command prompt) or through vswhere (the hosted CI runners).

.PARAMETER BuildDir
    Directory holding ddb.dll, ddbcmd.exe, gdal-util.exe and bm3d.exe (default: "build")

.EXAMPLE
    .\scripts\check-gpl-linkage.ps1 -BuildDir build

.NOTES
    Exit codes: 0 no GPL dependency, 1 violation, 2 the check could not run.
#>

param(
    [Parameter(HelpMessage = "Directory holding ddb.dll, ddbcmd.exe, gdal-util.exe and bm3d.exe")]
    [string]$BuildDir = "build"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# What each group of files must not import. Exiv2, bm3d and Qt are kept out of libddb and the
# CLI; Qt (5 or 6) is kept out of the bm3d helper.
$qtDll = '^Qt[56].*\.dll$'
$rules = @(
    @{ Files = @("ddb.dll", "ddbcmd.exe", "gdal-util.exe"); Forbidden = "^(lib)?(exiv2|bm3d)(\W.*)?\.dll$|$qtDll"
       Failure = "GPL code is linked into ddb.dll, ddbcmd.exe or gdal-util.exe; Exiv2 belongs to metadata-util.exe, and bm3d with its vcglib and corto code to bm3d.exe" },
    @{ Files = @("bm3d.exe"); Forbidden = $qtDll
       Failure = "Qt is back in bm3d.exe, which decodes textures with image codec libraries only" }
)

function Find-Dumpbin {
    $onPath = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' |
            Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

$dumpbin = Find-Dumpbin
if (-not $dumpbin) {
    Write-Host "error: dumpbin.exe not found: run from a Visual Studio command prompt or install the C++ build tools" -ForegroundColor Red
    exit 2
}

$failures = @()
foreach ($rule in $rules) {
    $violations = @()
    foreach ($name in $rule.Files) {
        $file = Join-Path $BuildDir $name
        if (-not (Test-Path $file)) {
            Write-Host "error: $file does not exist" -ForegroundColor Red
            exit 2
        }

        $output = & $dumpbin /nologo /dependents $file
        if ($LASTEXITCODE -ne 0) {
            Write-Host "error: dumpbin /dependents $file failed with exit code $LASTEXITCODE" -ForegroundColor Red
            exit 2
        }

        # The DLL names are the indented lines ending in .dll (also those of delay-loaded imports)
        $dependencies = @($output | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\S+\.dll$' })
        if ($dependencies.Count -eq 0) {
            Write-Host "error: dumpbin listed no dependencies for ${file}: the check would pass without proving anything" -ForegroundColor Red
            exit 2
        }

        Write-Host "$name imports: $($dependencies -join ', ')"
        foreach ($dependency in $dependencies) {
            if ($dependency -match $rule.Forbidden) {
                $violations += "${name}: imports $dependency"
            }
        }
    }
    foreach ($violation in $violations) { Write-Host "FAIL: $violation" -ForegroundColor Red }
    if ($violations.Count -gt 0) { $failures += $rule.Failure }
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Host "FAIL: $failure" -ForegroundColor Red }
    exit 1
}

Write-Host "PASS: no GPL code linked into ddb.dll, ddbcmd.exe or gdal-util.exe, no Qt in bm3d.exe" -ForegroundColor Green
exit 0
