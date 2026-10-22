<#
.SYNOPSIS
    Runs a test command and makes a hung test process diagnose itself (CI, Windows).

.DESCRIPTION
    Runs -Command in the foreground. Meanwhile a background thread watches the processes named
    -ProcessName: one that has run longer than -ThresholdSeconds gets a full memory dump in
    -DumpDir (procdump when it is on PATH, else the comsvcs.dll MiniDump that every Windows has)
    and, when the Windows SDK debugger cdb.exe is installed, the stacks of all its threads are
    printed into the log from that dump (symbols: _NT_SYMBOL_PATH, the PDBs next to the binaries
    and Microsoft's symbol server). -Kill then terminates it, for a command that has no timeout
    of its own. Each process is handled once; processes started before this script are left
    alone. The script exits with the command's exit code once the watcher has finished the dump
    it may be writing.

    Pick -ThresholdSeconds below the command's own timeout (ctest's TIMEOUT), so the dump is
    taken while the process still hangs. The workflow uploads -DumpDir when a step fails.

.EXAMPLE
    ./scripts/ci-hang-watchdog.ps1 -ProcessName ddb_unit_tests -ThresholdSeconds 240 `
        -DumpDir "$env:RUNNER_TEMP\ddbtest-dumps" -Command { ctest -C Release -L unit }
#>
param(
    [Parameter(Mandatory)] [string[]] $ProcessName,
    [Parameter(Mandatory)] [int] $ThresholdSeconds,
    [Parameter(Mandatory)] [string] $DumpDir,
    [Parameter(Mandatory)] [scriptblock] $Command,
    [switch] $Kill,
    [int] $PollSeconds = 5
)

$state = [hashtable]::Synchronized(@{ Stop = $false; Since = Get-Date })

$watcher = Start-ThreadJob -StreamingHost $Host -ArgumentList $ProcessName, $ThresholdSeconds, $DumpDir, $Kill.IsPresent, $PollSeconds, $state -ScriptBlock {
    param($names, $threshold, $dumpDir, $kill, $poll, $state)

    function Write-Dump($process, $dump) {
        $procdump = Get-Command procdump.exe -ErrorAction SilentlyContinue
        if ($procdump) {
            & $procdump.Source -accepteula -ma $process.Id $dump | Out-Host
        } else {
            # rundll32 is a GUI program: wait for it explicitly
            Start-Process rundll32.exe -Wait -NoNewWindow -ArgumentList "$env:SystemRoot\System32\comsvcs.dll, MiniDump $($process.Id) $dump full"
        }
        return (Test-Path $dump)
    }

    function Write-Stacks($dump) {
        $cdb = @("${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe",
                 "$env:ProgramFiles\Windows Kits\10\Debuggers\x64\cdb.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $cdb) {
            Write-Host "hang watchdog: cdb.exe not found, no stacks printed (open the dump artifact in WinDbg)"
            return
        }
        # Microsoft's symbols are cached outside dumpDir, which is uploaded
        $cache = Join-Path ([IO.Path]::GetTempPath()) 'ddb-symbols'
        $symbols = "$env:_NT_SYMBOL_PATH;srv*$cache*https://msdl.microsoft.com/download/symbols"
        Write-Host "::group::Thread stacks of $dump"
        & $cdb -z $dump -y $symbols -lines -c ".lastevent; ~*kn 100; q" | Out-Host
        Write-Host "::endgroup::"
    }

    New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
    $handled = @{}
    while (-not $state.Stop) {
        foreach ($process in Get-Process -Name $names -ErrorAction SilentlyContinue) {
            if ($handled.ContainsKey($process.Id)) { continue }
            try { $started = $process.StartTime } catch { continue } # gone
            $age = ((Get-Date) - $started).TotalSeconds
            if ($started -lt $state.Since -or $age -lt $threshold) { continue }
            $handled[$process.Id] = $true

            $dump = Join-Path $dumpDir ("{0}-{1}-hang.dmp" -f $process.ProcessName, $process.Id)
            Write-Host "::warning::hang watchdog: $($process.ProcessName) (pid $($process.Id)) has run $([int]$age) s, over $threshold s: full dump in $dump"
            try {
                if (Write-Dump $process $dump) {
                    Write-Stacks $dump
                } else {
                    Write-Host "::warning::hang watchdog: no dump written for pid $($process.Id)"
                }
            } catch {
                # The watcher goes on with the other processes
                Write-Host "::warning::hang watchdog: $_"
            }
            if ($kill) {
                Write-Host "::error::hang watchdog: terminating $($process.ProcessName) (pid $($process.Id))"
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
        Start-Sleep -Seconds $poll
    }
}

$code = 1
try {
    & $Command
    $code = $LASTEXITCODE
} finally {
    $state.Stop = $true
    Wait-Job $watcher | Out-Null
    Remove-Job $watcher
}
exit $code
