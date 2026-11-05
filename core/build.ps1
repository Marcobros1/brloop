# Builds the Metal Kart Wii core library tools with MSVC (cl.exe).
# Usage: powershell -ExecutionPolicy Bypass -File core\build.ps1
# Output: core\build\brstm_info.exe, core\build\brstm_decode.exe,
#         core\build\brstm_channels.exe
# Only PowerShell runs here: the VS developer environment script is not
# executed. Its variables (INCLUDE, LIB, PATH) are rebuilt from the VS and
# Windows SDK paths, then cl is called directly with a full path.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

# --- Toolchain discovery: same method as app\build.ps1 ----------------------
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

foreach ($tool in @($cl)) {
    if (-not (Test-Path $tool)) {
        Write-Error "Tool not found: $tool"
        exit 1
    }
}

# --- Build environment (no vcvars64.bat, no cmd) ----------------------------
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

# --- Sources and flags ------------------------------------------------------
$inc = Join-Path $root 'include'
$srcCommon = @(
    (Join-Path $root 'src\brstm.c'),
    (Join-Path $root 'src\dsp_adpcm.c'),
    (Join-Path $root 'src\wav_writer.c')
)
# /Zi: PDB so CI can measure these binaries with OpenCppCoverage (Q-103).
$flags = @('/nologo', '/W4', '/WX', '/O2', '/Zi', '/D_CRT_SECURE_NO_WARNINGS', ('/I' + $inc))
$tools = @('brstm_info', 'brstm_decode', 'brstm_channels')

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
    foreach ($tool in $tools) {
        Invoke-Native $cl (@($flags) + $srcCommon + (Join-Path $root "tools\$tool.c") + @('/Fe:' + $tool + '.exe'))
    }
}
finally {
    Pop-Location
}

# --- Fail the build if any executable is missing ----------------------------
foreach ($tool in $tools) {
    $exe = Join-Path $build "$tool.exe"
    if (-not (Test-Path $exe)) {
        Write-Error "Build finished but $exe is missing"
        exit 1
    }
}
Write-Host "Built: $build\brstm_info.exe, $build\brstm_decode.exe, $build\brstm_channels.exe"
