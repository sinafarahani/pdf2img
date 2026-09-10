# Downloads a pinned pdfium-binaries release (BSD-3) into third_party/pdfium.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/get_pdfium.ps1 [-Tag chromium/8009] [-Arch x64] [-Force]
param([string]$Tag = "chromium/8009", [string]$Arch = "x64", [switch]$Force)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root "third_party\pdfium"
if ((Test-Path "$dest\bin\pdfium.dll") -and -not $Force) { Write-Host "pdfium already present in $dest (use -Force to refetch)"; exit 0 }
$asset = "pdfium-win-$Arch.tgz"
$url = "https://github.com/bblanchon/pdfium-binaries/releases/download/$Tag/$asset"
$tmp = Join-Path $env:TEMP $asset
Write-Host "Downloading $url"
$ProgressPreference = "SilentlyContinue"
Invoke-WebRequest -Uri $url -OutFile $tmp
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $dest -Recurse -Force | Remove-Item -Recurse -Force
$tar = Join-Path $env:WINDIR "System32\tar.exe"   # use the Windows bsdtar, not an MSYS tar that misreads "C:"
& $tar -xzf $tmp -C $dest
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
Remove-Item $tmp
Set-Content -Path (Join-Path $dest "PINNED_RELEASE.txt") -Value "$Tag $asset"
Write-Host "pdfium extracted to $dest"
Get-Content (Join-Path $dest "VERSION")
