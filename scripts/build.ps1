# Builds pdf2img with CMake + Ninja inside the VS 2026 Build Tools environment.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/build.ps1 [-Config release|debug] [-Clean]
param([string]$Config = "release", [switch]$Clean)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vsBase = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
if (-not (Test-Path "$vsBase\Common7\Tools\Launch-VsDevShell.ps1")) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsBase = & $vswhere -latest -products * -property installationPath
}
Import-Module "$vsBase\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsBase -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$ninjaDir = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$cmakeDir = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$env:PATH = "$cmakeDir;$ninjaDir;$env:PATH"
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "H:\vcpkg" }
Set-Location $root
if ($Clean -and (Test-Path "build\$Config")) { Remove-Item -Recurse -Force "build\$Config" }
cmake --preset $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build --preset $Config
exit $LASTEXITCODE
