# Packages the BRLoop release zip from a fresh app build.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 -Version 0.1.0
# Output: dist\BRLoop-<Version>-win64.zip
param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'app\build'
$stage = Join-Path $root "dist\BRLoop-$Version-win64"
$zip = Join-Path $root "dist\BRLoop-$Version-win64.zip"

& powershell -ExecutionPolicy Bypass -File (Join-Path $root 'app\build.ps1')
if ($LASTEXITCODE -ne 0) {
    Write-Error "app build failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

$exe = Join-Path $build 'brloop.exe'
$font = Join-Path $build 'assets\fonts\PixelifySans.ttf'
$fontLicense = Join-Path $build 'assets\fonts\OFL.txt'
foreach ($required in @($exe, $font, $fontLicense, (Join-Path $root 'LICENSE'))) {
    if (-not (Test-Path $required)) {
        Write-Error "Missing release file: $required"
        exit 1
    }
}

if (Test-Path $stage) { Remove-Item -Path $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'assets\fonts') | Out-Null
Copy-Item -Path $exe -Destination $stage
Copy-Item -Path $font -Destination (Join-Path $stage 'assets\fonts')
Copy-Item -Path $fontLicense -Destination (Join-Path $stage 'assets\fonts')
Copy-Item -Path (Join-Path $root 'LICENSE') -Destination $stage

if (Test-Path $zip) { Remove-Item -Path $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Path $stage -Recurse -Force

Write-Host "Release: $zip"
