# Builds the Metal Kart Wii core library tools with MSVC (cl.exe).
# Usage: powershell -ExecutionPolicy Bypass -File core\build.ps1
# Output: core\build\brstm_info.exe, core\build\brstm_decode.exe
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
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
$inc = Join-Path $root 'include'
$srcCmd = (@(
    (Join-Path $root 'src\brstm.c'),
    (Join-Path $root 'src\dsp_adpcm.c'),
    (Join-Path $root 'src\wav_writer.c')
) | ForEach-Object { '"' + $_ + '"' }) -join ' '
$common = 'cl /nologo /W4 /WX /O2 /D_CRT_SECURE_NO_WARNINGS /I"' + $inc + '" ' + $srcCmd

# Note: every '+' expression needs explicit parentheses inside @( ... ) —
# PowerShell binds ',' tighter than '+', which silently splits elements.
$lines = @(
    '@echo off',
    ('call "' + $vcvars + '" >nul 2>&1'),
    ('if errorlevel 1 exit /b 1'),
    ('cd /d "' + $build + '"'),
    ($common + ' "' + (Join-Path $root 'tools\brstm_info.c') + '" /Fe:brstm_info.exe'),
    ('if errorlevel 1 exit /b 1'),
    ($common + ' "' + (Join-Path $root 'tools\brstm_decode.c') + '" /Fe:brstm_decode.exe'),
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
Write-Host "Built: $build\brstm_info.exe, $build\brstm_decode.exe"
