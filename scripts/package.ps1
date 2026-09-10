# Packages a build for release: <OutDir>/<Name>.zip on Windows, <OutDir>/<Name>.tar.gz on Linux and macOS.
# Usage: pwsh scripts/package.ps1 -BuildDir build/linux-release -Name pdf2img-v1.1.0-linux-x64 [-Triplet x64-linux]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Triplet = "",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$onWindows = [IO.Path]::DirectorySeparatorChar -eq '\'
$exe = if ($onWindows) { "pdf2img.exe" } else { "pdf2img" }
$lib = if ($onWindows) { "pdfium.dll" } elseif ($IsMacOS) { "libpdfium.dylib" } else { "libpdfium.so" }

if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $root $OutDir }
$stage = Join-Path $OutDir $Name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($f in $exe, $lib) {
    $src = Join-Path $BuildDir $f
    if (-not (Test-Path $src)) { throw "missing build output: $src" }
    Copy-Item $src $stage
}
Copy-Item (Join-Path $root "README.md") $stage
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") $stage
Copy-Item (Join-Path $root "docs/pdf2img.ini.example") $stage

# License texts: PDFium (and what it bundles), then every library vcpkg installed for this build.
$lic = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force (Join-Path $lic "pdfium") | Out-Null
$pdfium = Join-Path $root "third_party/pdfium"
Copy-Item (Join-Path $pdfium "LICENSE") (Join-Path $lic "pdfium/LICENSE.txt")
if (Test-Path (Join-Path $pdfium "licenses")) { Copy-Item (Join-Path $pdfium "licenses/*") (Join-Path $lic "pdfium") }
$installed = Join-Path $root "vcpkg_installed"
if (-not $Triplet) {
    $Triplet = Get-ChildItem $installed -Directory | Where-Object { $_.Name -ne "vcpkg" } | Select-Object -First 1 -ExpandProperty Name
}
$share = Join-Path $installed "$Triplet/share"
if (-not (Test-Path $share)) { throw "vcpkg share directory not found: $share" }
foreach ($port in Get-ChildItem $share -Directory) {
    $copyright = Join-Path $port.FullName "copyright"
    if ((Test-Path $copyright) -and $port.Name -notlike "vcpkg-*") { Copy-Item $copyright (Join-Path $lic "$($port.Name).txt") }
}

if ($onWindows) {
    $zip = Join-Path $OutDir "$Name.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $stage -DestinationPath $zip
    Write-Host "created $zip"
} else {
    chmod +x (Join-Path $stage $exe)
    $tgz = Join-Path $OutDir "$Name.tar.gz"
    tar -C $OutDir -czf $tgz $Name
    if ($LASTEXITCODE -ne 0) { throw "tar failed" }
    Write-Host "created $tgz"
}
Remove-Item $stage -Recurse -Force
