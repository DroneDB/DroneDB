#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Fails when ddb.dll or ddbcmd.exe depends on a GPL library (Exiv2, libnxs) or on Qt, or when
    ddb-nexus.exe or nxs.dll depends on Qt.

.DESCRIPTION
    Windows counterpart of scripts/check-gpl-linkage.py. Exiv2 is used only by the separate
    metadata-util.exe, and Nexus, vcglib and corto only by the separate ddb-nexus.exe (with nxs.dll);
    libddb starts both as child processes. A build whose ddb.dll or ddbcmd.exe imports exiv2.dll,
    nxs.dll or a Qt5*.dll again would link that code into the library. Qt is the other way round:
    ddb-nexus.exe and nxs.dll decode textures with plain image codec libraries and must not import
    a Qt DLL, which would bring back the Qt DLLs and plugins the package no longer carries.

    The imports are read with `dumpbin /dependents`, which is found on PATH (a Visual Studio
    command prompt) or through vswhere (the hosted CI runners).

.PARAMETER BuildDir
    Directory holding ddb.dll, ddbcmd.exe, ddb-nexus.exe and nxs.dll (default: "build")

.EXAMPLE
    .\scripts\check-gpl-linkage.ps1 -BuildDir build

.NOTES
    Exit codes: 0 no GPL dependency, 1 violation, 2 the check could not run.
#>

param(
    [Parameter(HelpMessage = "Directory holding ddb.dll, ddbcmd.exe, ddb-nexus.exe and nxs.dll")]
    [string]$BuildDir = "build"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# What each group of files must not import. Exiv2, libnxs (Nexus) and Qt are kept out of libddb and
# the CLI; Qt (5 or 6) is kept out of the Nexus helper.
$qtDll = '^Qt[56].*\.dll$'
$rules = @(
    @{ Files = @("ddb.dll", "ddbcmd.exe"); Forbidden = "^(lib)?(exiv2|nxs)(\W.*)?\.dll$|$qtDll"
       Failure = "GPL code is linked into ddb.dll or ddbcmd.exe; Exiv2 belongs to metadata-util and Nexus, vcglib and corto to ddb-nexus" },
    @{ Files = @("ddb-nexus.exe", "nxs.dll"); Forbidden = $qtDll
       Failure = "Qt is back in ddb-nexus.exe or nxs.dll, which decode textures with image codec libraries only" }
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

Write-Host "PASS: no GPL code linked into ddb.dll or ddbcmd.exe, no Qt in ddb-nexus.exe or nxs.dll" -ForegroundColor Green
exit 0
