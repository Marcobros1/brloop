# Builds the BRLoop desktop prototype (T2) with MSVC (cl.exe) and the Windows SDK (rc.exe).
# Usage: powershell -ExecutionPolicy Bypass -File app\build.ps1
# Output: app\build\brloop.exe, app\build\player_tests.exe
#
# Only PowerShell runs here: the Visual Studio developer environment script is
# not executed. Its variables (INCLUDE, LIB, PATH) are rebuilt from the Visual
# Studio and Windows SDK paths, then cl and rc are called directly with full paths.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $root
$core = Join-Path $repo 'core'
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

# --- Toolchain discovery -----------------------------------------------------
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

$msvcDir = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $msvcDir) {
    Write-Error "No MSVC toolset under $vs\VC\Tools\MSVC"
    exit 1
}
$msvc = $msvcDir.FullName
$clBin = Join-Path $msvc 'bin\Hostx64\x64'
$cl = Join-Path $clBin 'cl.exe'

$kitRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (-not $kitRoot) { $kitRoot = "${env:ProgramFiles(x86)}\Windows Kits\10" }
$kitVer = Get-ChildItem (Join-Path $kitRoot 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'um\windows.h')) } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $kitVer) {
    Write-Error "No Windows SDK headers (um\windows.h) under $kitRoot\Include"
    exit 1
}
$kit = $kitVer.Name
$rc = Join-Path $kitRoot "bin\$kit\x64\rc.exe"

foreach ($tool in @($cl, $rc)) {
    if (-not (Test-Path $tool)) {
        Write-Error "Tool not found: $tool"
        exit 1
    }
}

# --- Build environment (replaces the VS developer environment script) ----------
$env:INCLUDE = (@(
    (Join-Path $msvc 'include'),
    (Join-Path $kitRoot "Include\$kit\ucrt"),
    (Join-Path $kitRoot "Include\$kit\shared"),
    (Join-Path $kitRoot "Include\$kit\um"),
    (Join-Path $kitRoot "Include\$kit\winrt")
) -join ';')
$env:LIB = (@(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $kitRoot "Lib\$kit\ucrt\x64"),
    (Join-Path $kitRoot "Lib\$kit\um\x64")
) -join ';')
$env:PATH = $clBin + ';' + $env:PATH

# --- Sources, flags, libraries ------------------------------------------------
$incCore = Join-Path $core 'include'
$incApp = Join-Path $root 'include'
$srcCommon = @(
    (Join-Path $core 'src\brstm.c'),
    (Join-Path $core 'src\dsp_adpcm.c'),
    (Join-Path $root 'src\player.c'),
    (Join-Path $root 'src\audio_out.c'),
    (Join-Path $root 'src\cli_options.c'),
    (Join-Path $root 'src\library.c'),
    (Join-Path $root 'src\transport.c'),
    (Join-Path $root 'src\resampler.c'),
    (Join-Path $root 'src\strings.c'),
    (Join-Path $root 'src\settings.c')
)
# Drawing code needs GDI: only the window executable links it, so the headless
# test build stays free of GDI and cannot pick up window-only code by accident.
$srcUi = @(
    (Join-Path $root 'src\ui_selftest.c'),
    (Join-Path $root 'src\icons.c'),
    (Join-Path $root 'src\ui_state.c'),
    (Join-Path $root 'src\ui_layout.c'),
    (Join-Path $root 'src\ui_library.c')
)
# /utf-8: source and wide literals are UTF-8 (Spanish UI strings);
# UNICODE: wide Win32 API; winmm = waveOut, comctl32/comdlg32 = controls
# and the open-file dialog.
$playerFlags = @(
    '/nologo', '/W4', '/WX', '/O2', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS',
    '/DUNICODE', '/D_UNICODE',
    ('/I' + $incCore), ('/I' + $incApp)
)
$playerLink = @(
    '/link', 'user32.lib', 'gdi32.lib', 'shell32.lib', 'winmm.lib', 'comctl32.lib',
    'comdlg32.lib', 'psapi.lib', 'uxtheme.lib', 'advapi32.lib'
)
$testLink = @('/link', 'winmm.lib', 'psapi.lib')
$wav = Join-Path $core 'src\wav_writer.c'
$rcSrc = Join-Path $root 'src\brloop.rc'
$rcOut = Join-Path $build 'brloop.res'

function Invoke-Native {
    param([string]$Tool, [string[]]$ToolArgs)
    & $Tool @ToolArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "$([IO.Path]::GetFileName($Tool)) failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

Push-Location $build
try {
    # The icon resource is compiled to a .res and linked into the window executable
    # only. rc.exe ships with the Windows SDK.
    Invoke-Native $rc (@('/nologo', '/fo', $rcOut, $rcSrc))

    # /Fe must precede /link: after /link every token goes to the linker,
    # which ignores it and names the output after the first source file.
    Invoke-Native $cl (@($playerFlags) + $srcCommon + $srcUi + @((Join-Path $root 'src\main.c'), $rcOut, '/Fe:brloop.exe') + $playerLink)

    Invoke-Native $cl (@($playerFlags) + $srcCommon + @($wav, (Join-Path $root 'tests\player_tests.c'), '/Fe:player_tests.exe') + $testLink)
}
finally {
    Pop-Location
}

# --- Output check and assets ----------------------------------------------------
$exe = Join-Path $build 'brloop.exe'
if (-not (Test-Path $exe)) {
    Write-Error "Build finished but $exe is missing"
    exit 1
}

# The interface font is loaded from assets\fonts next to the executable, so the
# build output must carry it; otherwise the app falls back to the system font.
$fontDir = Join-Path $build 'assets\fonts'
New-Item -ItemType Directory -Force -Path $fontDir | Out-Null
Copy-Item -Path (Join-Path $root 'assets\fonts\*') -Destination $fontDir -Force
Write-Host "Built: $build\brloop.exe, $build\player_tests.exe"
