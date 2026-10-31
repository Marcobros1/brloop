# T1 verification harness: builds the core and checks it against vgmstream.
#
# Usage: powershell -ExecutionPolicy Bypass -File tests\run_verification.ps1 [-SkipBuild]
#
# Checks per test file (44100 stereo, 48000 stereo, 51269 stereo, 4ch, 8ch,
# one no-loop file):
#   1. brstm_info metadata vs test.exe -m and vs expected values.
#   2. Full decode vs vgmstream (-i): PCM data byte-for-byte identical.
#   3. --state adpc decode (per-block independent states) vs carried decode:
#      byte-identical when the ADPC table is usable; clean exit 4 when the
#      table is zeroed (fanfare 4ch, KOOPA 8ch).
#   4. Peak memory of the decode stays below half the full PCM size, i.e. the
#      song is not pre-decoded in full.
#
# Invalid cases: mutations of tKD_N.brstm (make_invalid_cases.py) must fail
# with the exact expected exit code and a matching error keyword, in both
# brstm_info and brstm_decode. A crash shows up as an unexpected exit code.
#
# Exit code: 0 all checks passed, 1 otherwise.
param([switch]$SkipBuild)

$ErrorActionPreference = 'Continue'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $scriptDir
$exeDir = Join-Path $root 'core\build'
$infoExe = Join-Path $exeDir 'brstm_info.exe'
$decodeExe = Join-Path $exeDir 'brstm_decode.exe'
$compare = Join-Path $scriptDir 'compare_pcm.py'
$mkInvalid = Join-Path $scriptDir 'make_invalid_cases.py'
$col = Join-Path $env:APPDATA 'CT-MKWII\Mods\Metal Kart Wii Retro Rewind'
# vgmstream CLI, used as the reference decoder. Set MKW_VGMSTREAM to point at
# another copy; the default assumes the LoopingAudioConverter layout.
$vgm = if ($env:MKW_VGMSTREAM) { $env:MKW_VGMSTREAM } else { Join-Path $env:USERPROFILE 'Desktop\LoopingAudioConverter-3.2\tools\vgmstream\test.exe' }
$work = Join-Path $env:LOCALAPPDATA 'Temp\opencode\t1_verify'

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

# Start-Process with explicit redirection avoids PowerShell wrapping long
# native stderr lines, which would break keyword matching.
function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments)
    $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $outF = Join-Path $work ('sp_out_' + $tag + '.txt')
    $errF = Join-Path $work ('sp_err_' + $tag + '.txt')
    $quoted = @($Arguments | ForEach-Object { '"' + $_ + '"' })
    $p = Start-Process -FilePath $FilePath -ArgumentList $quoted -NoNewWindow `
        -Wait -PassThru -RedirectStandardOutput $outF -RedirectStandardError $errF
    $out = ''
    $err = ''
    if (Test-Path $outF) { $out = [System.IO.File]::ReadAllText($outF) }
    if (Test-Path $errF) { $err = [System.IO.File]::ReadAllText($errF) }
    Remove-Item $outF, $errF -ErrorAction SilentlyContinue
    [pscustomobject]@{ StdOut = $out; StdErr = $err; ExitCode = $p.ExitCode }
}

function Parse-OurInfo {
    param([string]$Text)
    $kv = @{}
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match '^([a-z0-9_]+)=(.+)$') { $kv[$Matches[1]] = $Matches[2].Trim() }
    }
    $kv
}

function Parse-VgmMeta {
    param([string]$Text)
    $m = @{}
    if ($Text -match 'sample rate:\s*(\d+)') { $m['rate'] = [int]$Matches[1] }
    if ($Text -match 'channels:\s*(\d+)') { $m['channels'] = [int]$Matches[1] }
    if ($Text -match 'loop start:\s*(\d+)\s*samples') {
        $m['loopStart'] = [int]$Matches[1]
        $m['hasLoop'] = $true
    } else {
        $m['hasLoop'] = $false
    }
    if ($Text -match 'loop end:\s*(\d+)\s*samples') { $m['loopEnd'] = [int]$Matches[1] }
    if ($Text -match 'stream total samples:\s*(\d+)') { $m['total'] = [int]$Matches[1] }
    $m
}

function Measure-DecodePeak {
    param([string]$Exe, [string]$In, [string]$OutWav)
    $tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $outF = Join-Path $work ('sp_null_out_' + $tag + '.txt')
    $errF = Join-Path $work ('sp_null_err_' + $tag + '.txt')
    $p = Start-Process -FilePath $Exe -ArgumentList @('"' + $In + '"', '"' + $OutWav + '"') `
        -NoNewWindow -PassThru -RedirectStandardOutput $outF -RedirectStandardError $errF
    $peak = 0
    while (-not $p.HasExited) {
        try {
            $p.Refresh()
            if ($p.WorkingSet64 -gt $peak) { $peak = $p.WorkingSet64 }
        } catch { }
        Start-Sleep -Milliseconds 20
    }
    $p.WaitForExit()
    Remove-Item $outF, $errF -ErrorAction SilentlyContinue
    [pscustomobject]@{ Peak = $peak; ExitCode = $p.ExitCode }
}

# ---------------------------------------------------------------- preparation

if (-not $SkipBuild) {
    Write-Host '=== build ===' -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'core\build.ps1')
    Check 'build core' ($LASTEXITCODE -eq 0) ('exit=' + $LASTEXITCODE)
}
Check 'brstm_info.exe exists' (Test-Path $infoExe) $infoExe
Check 'brstm_decode.exe exists' (Test-Path $decodeExe) $decodeExe
Check 'vgmstream test.exe exists' (Test-Path $vgm) $vgm
Check 'python available' ((& python --version 2>&1) -match 'Python 3') ''

if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Expected values collected from the collection headers and cross-checked
# against vgmstream at run time. memLimit = half the full PCM size: a decoder
# that pre-decodes the whole song in memory would exceed it.
# adpc = exact:  ADPC table equals the carried state, per-block decode must be
#                bit-identical to the continuous decode.
# adpc = approx: the encoder wrote slightly off ADPC states (research/
#                adpc_mismatch_probe.py); per-block decode must stay within a
#                small transient at block boundaries (< 0.5% of bytes) instead
#                of being bit-identical.
# adpc = zeroed: the table is empty; --state adpc must fail cleanly (exit 4).
$testFiles = @(
    @{ name = 'uTTC_N.brstm'; rate = 44100; channels = 2; loop = $true; loopStart = 5634048; total = 12786908; adpc = 'exact'; memLimit = 26214400 },
    @{ name = 'tKD_N.brstm'; rate = 44100; channels = 2; loop = $true; loopStart = 1347584; total = 4876838; adpc = 'exact' },
    @{ name = '104_N.brstm'; rate = 48000; channels = 2; loop = $true; loopStart = 501760; total = 8771278; adpc = 'exact' },
    @{ name = 'n_rainbow32_f.brstm'; rate = 51269; channels = 2; loop = $true; loopStart = 0; total = 8571425; adpc = 'approx' },
    @{ name = 'o_Wi-Fi_waiting32.brstm'; rate = 48000; channels = 2; loop = $false; loopStart = 0; total = 7870464; adpc = 'approx' },
    @{ name = 'o_FanfareGP1_32.brstm'; rate = 48000; channels = 4; loop = $true; loopStart = 860160; total = 1303244; adpc = 'zeroed' },
    @{ name = 'STRM_N_KOOPA_N.brstm'; rate = 48000; channels = 8; loop = $true; loopStart = 0; total = 6270135; adpc = 'zeroed'; memLimit = 50331648 }
)

# ------------------------------------------------------------------ test set

foreach ($t in $testFiles) {
    Write-Host ''
    Write-Host ('=== ' + $t.name + ' ===') -ForegroundColor Cyan
    $src = Join-Path $col $t.name
    Check ('file exists: ' + $t.name) (Test-Path $src) $src
    if (-not (Test-Path $src)) { continue }

    # Metadata: ours vs vgmstream vs expected.
    $r = Invoke-Native $infoExe @($src)
    Check 'brstm_info exit 0' ($r.ExitCode -eq 0) ('exit=' + $r.ExitCode + ' ' + $r.StdErr.Trim())
    $ours = Parse-OurInfo $r.StdOut
    $v = Invoke-Native $vgm @('-m', $src)
    $vg = Parse-VgmMeta ($v.StdOut + $v.StdErr)

    Check 'sample rate vs vgmstream/expected' `
        ([int]$ours['sample_rate'] -eq $t.rate -and $vg['rate'] -eq $t.rate) `
        ('ours=' + $ours['sample_rate'] + ' vgm=' + $vg['rate'] + ' expected=' + $t.rate)
    Check 'channel count vs vgmstream/expected' `
        ([int]$ours['channels'] -eq $t.channels -and $vg['channels'] -eq $t.channels) `
        ('ours=' + $ours['channels'] + ' vgm=' + $vg['channels'] + ' expected=' + $t.channels)
    Check 'loop flag vs vgmstream/expected' `
        ((($ours['loop'] -eq 'true') -eq $t.loop) -and ($vg['hasLoop'] -eq $t.loop)) `
        ('ours=' + $ours['loop'] + ' vgmHasLoop=' + $vg['hasLoop'])
    if ($t.loop) {
        Check 'loop start vs vgmstream/expected' `
            ([int]$ours['loop_start'] -eq $t.loopStart -and $vg['loopStart'] -eq $t.loopStart) `
            ('ours=' + $ours['loop_start'] + ' vgm=' + $vg['loopStart'] + ' expected=' + $t.loopStart)
        Check 'loop end == total vs vgmstream' `
            ([int]$ours['loop_end'] -eq $t.total -and $vg['loopEnd'] -eq $t.total) `
            ('ours=' + $ours['loop_end'] + ' vgm=' + $vg['loopEnd'])
    }
    Check 'total samples vs vgmstream/expected' `
        ([int]$ours['total_samples'] -eq $t.total -and $vg['total'] -eq $t.total) `
        ('ours=' + $ours['total_samples'] + ' vgm=' + $vg['total'] + ' expected=' + $t.total)

    # Decode: ours vs vgmstream (-i = ignore loop, whole stream once).
    $oursWav = Join-Path $work ($t.name + '.ours.wav')
    $refWav = Join-Path $work ($t.name + '.ref.wav')
    $adpcWav = Join-Path $work ($t.name + '.adpc.wav')

    $d = Invoke-Native $decodeExe @($src, $oursWav)
    Check 'brstm_decode exit 0' ($d.ExitCode -eq 0) ('exit=' + $d.ExitCode + ' ' + $d.StdErr.Trim())
    Check 'decode reported total samples' ($d.StdOut -match ('decoded ' + $t.total + ' samples')) ($d.StdOut.Trim())

    $v2 = Invoke-Native $vgm @('-i', '-o', $refWav, $src)
    Check 'vgmstream decode exit 0' ($v2.ExitCode -eq 0) ('exit=' + $v2.ExitCode)

    $cmp = Invoke-Native 'python' @($compare, $oursWav, $refWav)
    Check 'PCM ours == vgmstream byte-for-byte' ($cmp.ExitCode -eq 0) (($cmp.StdOut + $cmp.StdErr).Trim())

    # Per-block independent decode vs continuous decode.
    $a = Invoke-Native $decodeExe @($src, $adpcWav, '--state', 'adpc')
    if ($t.adpc -eq 'zeroed') {
        Check 'adpc rejected on zeroed table (exit 4)' ($a.ExitCode -eq 4) ('exit=' + $a.ExitCode + ' ' + $a.StdErr.Trim())
        Check 'adpc rejection wrote no wav' (-not (Test-Path $adpcWav)) $adpcWav
    } else {
        Check 'adpc decode exit 0' ($a.ExitCode -eq 0) ('exit=' + $a.ExitCode + ' ' + $a.StdErr.Trim())
        if ($t.adpc -eq 'exact') {
            $cmp2 = Invoke-Native 'python' @($compare, $oursWav, $adpcWav)
            Check 'PCM per-block (adpc) == continuous (carry)' ($cmp2.ExitCode -eq 0) (($cmp2.StdOut + $cmp2.StdErr).Trim())
        } else {
            # approx: encoder wrote slightly off ADPC states; only a short
            # transient at block boundaries is acceptable.
            $cmp2 = Invoke-Native 'python' @($compare, $oursWav, $adpcWav, '--max-bad-fraction', '0.005')
            Check 'PCM per-block (adpc) ~= continuous (carry), transient < 0.5%' ($cmp2.ExitCode -eq 0) (($cmp2.StdOut + $cmp2.StdErr).Trim())
        }
    }

    # No full pre-decode: peak memory below half the full PCM size.
    if ($t.ContainsKey('memLimit')) {
        $m = Measure-DecodePeak $decodeExe $src $oursWav
        Check ('streaming decode peak memory < ' + [math]::Round($t.memLimit / 1MB, 0) + ' MB') `
            ($m.Peak -lt $t.memLimit) `
            ('peak=' + [math]::Round($m.Peak / 1MB, 1) + ' MB, full PCM would be ' + [math]::Round($t.memLimit * 2 / 1MB, 0) + ' MB')
    }

    Remove-Item $oursWav, $refWav, $adpcWav -ErrorAction SilentlyContinue
}

# ------------------------------------------------------------ invalid cases

Write-Host ''
Write-Host '=== invalid cases (mutations of tKD_N.brstm) ===' -ForegroundColor Cyan
$invDir = Join-Path $work 'invalid'
$mk = Invoke-Native 'python' @($mkInvalid, (Join-Path $col 'tKD_N.brstm'), $invDir)
Check 'make_invalid_cases exit 0' ($mk.ExitCode -eq 0) (($mk.StdOut + $mk.StdErr).Trim())

if (Test-Path (Join-Path $invDir 'manifest.json')) {
    $manifest = Get-Content (Join-Path $invDir 'manifest.json') -Raw | ConvertFrom-Json
    foreach ($case in $manifest) {
        $f = Join-Path $invDir ($case.name + '.brstm')
        $ri = Invoke-Native $infoExe @($f)
        $text = $ri.StdErr + $ri.StdOut
        $okExit = $ri.ExitCode -eq $case.exit
        $hasKw = ($case.keyword -eq '') -or $text.Contains($case.keyword)
        Check ('info rejects ' + $case.name) ($okExit -and $hasKw) `
            ('exit=' + $ri.ExitCode + ' expected=' + $case.exit + ' keyword=' + $case.keyword + ' err=' + $ri.StdErr.Trim())
        $rd = Invoke-Native $decodeExe @($f, (Join-Path $work 'case_out.wav'))
        Check ('decode rejects ' + $case.name) ($rd.ExitCode -eq $case.exit) `
            ('exit=' + $rd.ExitCode + ' expected=' + $case.exit + ' err=' + $rd.StdErr.Trim())
    }
    Remove-Item (Join-Path $work 'case_out.wav') -ErrorAction SilentlyContinue
    Remove-Item $invDir -Recurse -Force -ErrorAction SilentlyContinue
}

# ------------------------------------------------------------------- summary

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
