# Builds the Metal Kart Wii desktop prototype (T2) with MSVC (cl.exe).
# Usage: powershell -ExecutionPolicy Bypass -File app\build.ps1
# Output: app\build\brstm_player.exe, app\build\player_tests.exe
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $root
$core = Join-Path $repo 'core'
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error 'vswhere.exe not found; Visual Studio 2022 with C++ tools is required'
    exit 1
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    Write-Error 'No Visual Studio installation with C++ x64 tools found'
    exit 1
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
    exit 1
}

# Generate a batch file: PowerShell's native argument quoting mangles nested
# quotes with spaces, a .bat avoids that entirely.
$incCore = Join-Path $core 'include'
$incApp = Join-Path $root 'include'
$srcCommon = (@(
    (Join-Path $core 'src\brstm.c'),
    (Join-Path $core 'src\dsp_adpcm.c'),
    (Join-Path $root 'src\player.c'),
    (Join-Path $root 'src\audio_out.c'),
    (Join-Path $root 'src\cli_options.c'),
    (Join-Path $root 'src\library.c'),
    (Join-Path $root 'src\transport.c'),
    (Join-Path $root 'src\resampler.c')
) | ForEach-Object { '"' + $_ + '"' }) -join ' '
$srcUi = '"' + (Join-Path $root 'src\ui_selftest.c') + '"'
# /utf-8: source and wide literals are UTF-8 (Spanish UI strings);
# UNICODE: wide Win32 API; winmm = waveOut, comctl32/comdlg32 = controls
# and the open-file dialog.
$playerFlags = 'cl /nologo /W4 /WX /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS ' +
               '/DUNICODE /D_UNICODE /I"' + $incCore + '" /I"' + $incApp + '" '
$playerLink = ' /link user32.lib gdi32.lib shell32.lib winmm.lib comctl32.lib comdlg32.lib psapi.lib'
$testLink = ' /link winmm.lib psapi.lib'
$wav = '"' + (Join-Path $core 'src\wav_writer.c') + '"'

# Note: every '+' expression needs explicit parentheses inside @( ... ) —
# PowerShell binds ',' tighter than '+', which silently splits elements.
$lines = @(
    '@echo off',
    ('call "' + $vcvars + '" >nul 2>&1'),
    ('if errorlevel 1 exit /b 1'),
    ('cd /d "' + $build + '"'),
    # /Fe must precede /link: after /link every token goes to the linker,
    # which ignores it and names the output after the first source file.
    ($playerFlags + $srcCommon + ' ' + $srcUi + ' "' +
     (Join-Path $root 'src\main.c') + '"' +
     ' /Fe:brstm_player.exe' + $playerLink),
    ('if errorlevel 1 exit /b 1'),
    ($playerFlags + $srcCommon + ' ' + $wav + ' "' +
     (Join-Path $root 'tests\player_tests.c') + '"' +
     ' /Fe:player_tests.exe' + $testLink),
    ('if errorlevel 1 exit /b 1')
)
$bat = Join-Path $build 'build.bat'
Set-Content -Path $bat -Value $lines -Encoding Ascii

& cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) {
    Write-Error "Compilation failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}
Remove-Item -Path $bat -Force
Write-Host "Built: $build\brstm_player.exe, $build\player_tests.exe"
