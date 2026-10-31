# T2 verification harness: one command that covers every mandatory test case.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tests\run_t2_verification.ps1
#
# Steps:
#   1. T1 harness (core parser/decoder) must pass before any T2 work runs.
#   2. SHA-256 of every .brstm in the reference collection (baseline).
#   3. Build the app (brloop.exe + player_tests.exe).
#   4. Regenerate the invalid mutants from tKD_N.brstm.
#   5. player_tests full battery:
#        metadata (44100/48000/51269, 4ch, 8ch, no-loop) speed (FLSM mapping)
#        invalid (38 mutants rejected)      loop (byte-identical vs vgmstream)
#        noloop (plays to the end)          loopoff (loop flag off -> end)
#        memory (stable over 16M looped frames)  th (transport, headless)
#        probe (handle balance)             ta (real device transport, 4ch fold)
#        realtime (1.0x vs FLSM duration, live loop wrap)
#   6. brloop --selftest: real window, real handlers, invalid file
#      rejected, track change, pause/resume, FLSM speed, close releases
#      resources.
#   7. SHA-256 of the collection again: playback must not modify originals.
#   8. T1 harness again: the core is still unchanged and passing.
#
# Exit code: 0 all checks passed, 1 otherwise.

$ErrorActionPreference = 'Continue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDir
$appDir = Join-Path $root 'app'
$testsExe = Join-Path $appDir 'build\player_tests.exe'
$playerExe = Join-Path $appDir 'build\brloop.exe'
$mkInvalid = Join-Path $scriptDir 'make_invalid_cases.py'
$t1Script = Join-Path $scriptDir 'run_verification.ps1'
$col = Join-Path $env:APPDATA 'CT-MKWII\Mods\Metal Kart Wii Retro Rewind'
# vgmstream CLI, used as the reference decoder. Set MKW_VGMSTREAM to point at
# another copy; the default assumes the LoopingAudioConverter layout.
$vgm = if ($env:MKW_VGMSTREAM) { $env:MKW_VGMSTREAM } else { Join-Path $env:USERPROFILE 'Desktop\LoopingAudioConverter-3.2\tools\vgmstream\test.exe' }
$work = Join-Path $env:LOCALAPPDATA 'Temp\opencode\t2_verify'

# Every test run sends silence to the device (the path stays the same), so the
# battery never plays sound. Normal use never sets this variable.
$env:MKW_SILENT_AUDIO = '1'

$script:pass = 0
$script:fail = 0
$script:failures = @()

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    if ($Ok) {
        $script:pass++
        Write-Host ('  [PASS] ' + $Name) -ForegroundColor Green
    } else {
        $script:fail++
        $script:failures += ($Name + $(if ($Detail) { ': ' + $Detail } else { '' }))
        Write-Host ('  [FAIL] ' + $Name + ' ' + $Detail) -ForegroundColor Red
    }
}

function Invoke-Logged {
    # Runs a script/exe with stdout+stderr captured to files and the exit
    # code returned; the log path is echoed for diagnosis.
    param([string]$FilePath, [string[]]$Arguments, [string]$Log)
    $quoted = @($Arguments | ForEach-Object { '"' + $_ + '"' })
    & $FilePath @quoted *> $Log
    $LASTEXITCODE
}

# ------------------------------------------------------------ 1. T1 before

Write-Host '=== 1. T1 harness (before) ===' -ForegroundColor Cyan
$t1a = Join-Path $work 't1_before.txt'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$t1aCode = Invoke-Logged 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', $t1Script) $t1a
Check 'T1 verification passes before T2' ($t1aCode -eq 0) ('exit=' + $t1aCode + ' log=' + $t1a)

# ------------------------------------------------- 2. SHA-256 baseline

Write-Host '=== 2. SHA-256 of the reference collection (before) ===' -ForegroundColor Cyan
$before = @{}
$files = @(Get-ChildItem -Path $col -Recurse -Filter '*.brstm' -File -ErrorAction SilentlyContinue)
Check 'collection .brstm files found' ($files.Count -gt 0) ('count=' + $files.Count)
foreach ($f in $files) { $before[$f.FullName] = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash }
Check 'SHA-256 baseline taken' ($before.Count -eq $files.Count) ($before.Count.ToString() + '/' + $files.Count)

# ------------------------------------------------------- 3. build + mutants

Write-Host '=== 3. build the app ===' -ForegroundColor Cyan
$buildLog = Join-Path $work 'build.txt'
$buildCode = Invoke-Logged 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $appDir 'build.ps1')) $buildLog
Check 'app build succeeds' ($buildCode -eq 0) ('exit=' + $buildCode + ' log=' + $buildLog)
Check 'player_tests.exe exists' (Test-Path $testsExe) $testsExe
Check 'brloop.exe exists' (Test-Path $playerExe) $playerExe
Check 'vgmstream test.exe exists' (Test-Path $vgm) $vgm

Write-Host '=== 4. invalid mutants ===' -ForegroundColor Cyan
$invDir = Join-Path $work 'invalid'
if (Test-Path $invDir) { Remove-Item $invDir -Recurse -Force }
$mkLog = Join-Path $work 'make_invalid.txt'
$mkCode = Invoke-Logged 'python' @($mkInvalid, (Join-Path $col 'tKD_N.brstm'), $invDir) $mkLog
Check 'make_invalid_cases exit 0' ($mkCode -eq 0) ('exit=' + $mkCode + ' log=' + $mkLog)
$mutants = @(Get-ChildItem -Path $invDir -Filter '*.brstm' -ErrorAction SilentlyContinue)
Check 'mutants generated' ($mutants.Count -ge 30) ('count=' + $mutants.Count)
$badFile = Join-Path $invDir 'bad_magic.brstm'
Check 'bad_magic.brstm available for the UI selftest' (Test-Path $badFile) $badFile

# ------------------------------------------------- 5. player_tests battery

Write-Host '=== 5. player_tests full battery ===' -ForegroundColor Cyan
$ptReport = Join-Path $work 'player_tests_report.txt'
$ptLog = Join-Path $work 'player_tests.txt'
$ptArgs = @('--collection', $col, '--invalid', $invDir, '--vgmstream', $vgm,
            '--work', $work, '--report', $ptReport)
$ptCode = Invoke-Logged $testsExe $ptArgs $ptLog
$ptText = ''
if (Test-Path $ptLog) { $ptText = [System.IO.File]::ReadAllText($ptLog) }
Check 'player_tests exit 0' ($ptCode -eq 0) ('exit=' + $ptCode + ' log=' + $ptLog)
Check 'player_tests RESULT: PASS' ($ptText -match 'RESULT: PASS') ($ptText.Trim().Split("`n")[-1])
$failLines = @($ptText -split "`r?`n" | Where-Object { $_ -match '^\[FAIL\]' })
Check 'no [FAIL] line in the battery' ($failLines.Count -eq 0) ($failLines -join ' | ')

# -------------------------------------- 6. UI selftest (real window/handlers)

Write-Host '=== 6. brloop --selftest ===' -ForegroundColor Cyan
$stReport = Join-Path $work 'selftest_report.txt'
$stLog = Join-Path $work 'selftest_stdout.txt'
$stErr = Join-Path $work 'selftest_stderr.txt'
# The self-test writes its language choice to MKW_SETTINGS_DIR, never to the
# user's %APPDATA%: a test run must not change the preference of the person
# running it.
$env:MKW_SETTINGS_DIR = Join-Path $work 'settings'
$stArgs = @('--selftest', (Join-Path $col 'tKD_N.brstm'),
            (Join-Path $col 'o_Crs_In_Fan_Wifi.brstm'),
            '--invalid', $badFile, '--report', $stReport)
$quoted = @($stArgs | ForEach-Object { '"' + $_ + '"' })
$stProc = Start-Process -FilePath $playerExe -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $stLog -RedirectStandardError $stErr
Check 'selftest exit 0' ($stProc.ExitCode -eq 0) ('exit=' + $stProc.ExitCode + ' report=' + $stReport)
$stText = ''
if (Test-Path $stReport) { $stText = [System.IO.File]::ReadAllText($stReport) }
Check 'selftest RESULT: PASS' ($stText -match 'RESULT: PASS') $stText.Trim().Split("`n")[-1]
Check 'selftest rejected the invalid file' ($stText -match 'invalid file rejected') ''
Check 'selftest closed without leaking handles' ($stText -match 'closing the window releases resources') ''

# ------------------------------------------- 7. SHA-256 after playback

Write-Host '=== 7. SHA-256 after playback (originals untouched) ===' -ForegroundColor Cyan
$modified = @()
foreach ($f in $files) {
    $h = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash
    if ($before[$f.FullName] -ne $h) { $modified += $f.FullName }
}
Check 'collection files unchanged after all playback' ($modified.Count -eq 0) ($modified -join ', ')

# ------------------------------------------------------ 8. T1 after

Write-Host '=== 8. T1 harness (after) ===' -ForegroundColor Cyan
$t1b = Join-Path $work 't1_after.txt'
# No -SkipBuild: rebuilding here proves the core still compiles unchanged
# after all T2 work, and keeps both runs symmetric (180 checks).
$t1bCode = Invoke-Logged 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', $t1Script) $t1b
Check 'T1 verification passes after T2' ($t1bCode -eq 0) ('exit=' + $t1bCode + ' log=' + $t1b)

# ------------------------------------------------------------- summary

Write-Host ''
Write-Host '=== summary ===' -ForegroundColor Cyan
Write-Host ('passed: ' + $script:pass)
Write-Host ('failed: ' + $script:fail)
foreach ($f in $script:failures) {
    Write-Host ('  FAIL: ' + $f) -ForegroundColor Red
}
if ($script:fail -eq 0) {
    Write-Host ('RESULT: PASS (' + $script:pass + ' checks)') -ForegroundColor Green
    exit 0
}
Write-Host ('RESULT: FAIL (' + $script:fail + ' of ' + ($script:pass + $script:fail) + ' checks)') -ForegroundColor Red
exit 1
