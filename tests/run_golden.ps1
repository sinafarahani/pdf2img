# Replays the cases captured from the old VeryPDF tool (tests/golden/golden.json) against pdf2img and compares the
# exit code, the produced file names, pixel size, bit depth, frame count, resolution and TIFF tags.
# Works with Windows PowerShell 5.1 and PowerShell 7 on Windows, Linux and macOS (images are inspected by parsing
# their headers, no imaging library needed).
# Usage: pwsh tests/run_golden.ps1 [-Exe build/linux-release/pdf2img] [-Only A01_one_tif,B01_r300] [-Quiet]
param([string]$Exe = "", [string[]]$Only, [switch]$Quiet)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$onWindows = [IO.Path]::DirectorySeparatorChar -eq '\'
if (-not $Exe) {
    $Exe = @("build/release/pdf2img.exe", "build/linux-release/pdf2img", "build/macos-release/pdf2img") |
        ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Exe) { throw "no build found; pass -Exe" }
}
$Exe = (Resolve-Path $Exe).Path
$exeDir = Split-Path $Exe -Parent
$golden = Get-Content (Join-Path $PSScriptRoot "golden/golden.json") -Raw -Encoding UTF8 | ConvertFrom-Json
$pdfDir = Join-Path $PSScriptRoot "golden/pdfs"
$outRoot = Join-Path $PSScriptRoot "out/golden"
New-Item -ItemType Directory -Force $outRoot | Out-Null

# Persian names for the Unicode case, built from code points so that the script's own encoding does not matter.
function Chars([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }
$faDir = Chars 0x067E, 0x0648, 0x0634, 0x0647, 0x20, 0x062A, 0x0633, 0x062A     # "test folder"
$faIn = Chars 0x0648, 0x0631, 0x0648, 0x062F, 0x06CC, 0x20, 0x062A, 0x0633, 0x062A # "test input"
$faOut = Chars 0x062E, 0x0631, 0x0648, 0x062C, 0x06CC                            # "output"

# Deliberate behaviour differences of the rewrite (see docs/DESIGN.md, "Deliberate differences").
$expectOverride = @{
    "A08_three_wmf"    = @{ exit = 6; files = @() }                 # WMF/EMF not supported -> error, no files
    "A13_three_noext"  = @{ exit = 6; files = @() }                 # no extension -> error instead of 0-byte files
    "A14_three_badext" = @{ exit = 6; files = @() }                 # unknown extension -> error
    "A17_nodir"        = @{ exit = 0; files = @("nodir/sub/out0001.tif", "nodir/sub/out0002.tif", "nodir/sub/out0003.tif"); like = "A12_three_no_o" }
    "C01_missing"      = @{ exit = 2; files = @() }
    "C03_enc"          = @{ exit = 7; files = @() }                 # user password required
    "C10_corrupt"      = @{ exit = 2; files = @() }
    "C18_overwrite"    = @{ exit = 0; files = @("out0001.tif", "out0002.tif") }  # out0002.tif is the stale garbage file (left alone, like the old tool)
    "C19_unicode"      = @{ exit = 0; files = @("$faDir/${faOut}0001.tif"); like = "A01_one_tif" }
}
# Per-file attribute differences accepted by design.
function Normalize-Expected($f) {
    $e = [ordered]@{}
    foreach ($p in $f.PSObject.Properties) { $e[$p.Name] = $p.Value }
    foreach ($k in "tiff_fillOrder", "tiff_rowsPerStrip", "size") { if ($e.Contains($k)) { $e.Remove($k) } } # different renderer/compressor
    return $e
}

# ---------------------------------------------------------------- header parsers
function U16([byte[]]$b, [long]$o, [bool]$le) { if ($le) { [long]$b[$o] + 256L * $b[$o + 1] } else { 256L * $b[$o] + $b[$o + 1] } }
function U32([byte[]]$b, [long]$o, [bool]$le) {
    if ($le) { [long]$b[$o] + 256L * $b[$o + 1] + 65536L * $b[$o + 2] + 16777216L * $b[$o + 3] }
    else { 16777216L * $b[$o] + 65536L * $b[$o + 1] + 256L * $b[$o + 2] + [long]$b[$o + 3] }
}
function Indexed([long]$bits) { "Format$($bits)bppIndexed" }

function Inspect-Tiff([byte[]]$b) {
    $le = $b[0] -eq 0x49
    $r = [ordered]@{}
    $off = U32 $b 4 $le
    $frames = 0
    while ($off -gt 0 -and $off + 2 -le $b.Length -and $frames -lt 100000) {
        $n = U16 $b $off $le
        if ($frames -eq 0) {
            for ($i = 0; $i -lt $n; $i++) {
                $e = $off + 2 + 12 * $i
                $tag = U16 $b $e $le; $type = U16 $b ($e + 2) $le; $count = U32 $b ($e + 4) $le
                $v = if ($type -eq 3) { if ($count -le 2) { U16 $b ($e + 8) $le } else { U16 $b (U32 $b ($e + 8) $le) $le } }
                     elseif ($type -eq 4) { U32 $b ($e + 8) $le }
                     elseif ($type -eq 5) { $ro = U32 $b ($e + 8) $le; $den = U32 $b ($ro + 4) $le; if ($den) { [math]::Round((U32 $b $ro $le) / $den, 2) } else { 0 } }
                     else { $null }
                switch ($tag) {
                    256 { $r.width = [int]$v }
                    257 { $r.height = [int]$v }
                    258 { $r.tiff_bitsPerSample = [int]$v }
                    259 { $r.tiff_compression = [int]$v }
                    262 { $r.tiff_photometric = [int]$v }
                    277 { $r.tiff_samplesPerPixel = [int]$v }
                    282 { $r.dpiX = $v }
                    283 { $r.dpiY = $v }
                }
            }
        }
        $frames++
        $off = U32 $b ($off + 2 + 12 * $n) $le
    }
    $r.frames = $frames
    $spp = if ($r.Contains("tiff_samplesPerPixel")) { $r.tiff_samplesPerPixel } else { 1 }
    $bps = if ($r.Contains("tiff_bitsPerSample")) { $r.tiff_bitsPerSample } else { 1 }
    $r.pixelFormat = if ($spp -eq 3) { "Format24bppRgb" } elseif ($spp -eq 4) { "Format32bppArgb" } else { Indexed $bps }
    return $r
}

function Inspect-Png([byte[]]$b) {
    $r = [ordered]@{ width = [int](U32 $b 16 $false); height = [int](U32 $b 20 $false); frames = 1 }
    $depth = $b[24]; $type = $b[25]
    $r.pixelFormat = switch ($type) { 2 { "Format24bppRgb" } 6 { "Format32bppArgb" } default { Indexed $depth } }
    $pos = 8
    while ($pos + 8 -le $b.Length) {
        $len = U32 $b $pos $false
        $name = [Text.Encoding]::ASCII.GetString($b, [int]$pos + 4, 4)
        if ($name -eq "pHYs" -and $b[$pos + 16] -eq 1) {
            $r.dpiX = [math]::Round((U32 $b ($pos + 8) $false) * 0.0254, 2)
            $r.dpiY = [math]::Round((U32 $b ($pos + 12) $false) * 0.0254, 2)
        }
        if ($name -eq "IEND") { break }
        $pos += 12 + $len
    }
    return $r
}

function Inspect-Jpeg([byte[]]$b) {
    $r = [ordered]@{ frames = 1 }
    $pos = 2
    while ($pos + 4 -le $b.Length) {
        if ($b[$pos] -ne 0xFF) { $pos++; continue }
        $m = $b[$pos + 1]
        if ($m -eq 0xD8 -or $m -eq 0x01 -or ($m -ge 0xD0 -and $m -le 0xD7)) { $pos += 2; continue }
        $len = U16 $b ($pos + 2) $false
        if ($m -eq 0xE0 -and [Text.Encoding]::ASCII.GetString($b, [int]$pos + 4, 4) -eq "JFIF") {
            $units = $b[$pos + 11]; $x = U16 $b ($pos + 12) $false; $y = U16 $b ($pos + 14) $false
            $f = if ($units -eq 2) { 2.54 } else { 1 }
            if ($units -ne 0) { $r.dpiX = [math]::Round($x * $f, 2); $r.dpiY = [math]::Round($y * $f, 2) }
        }
        if ($m -ge 0xC0 -and $m -le 0xCF -and $m -ne 0xC4 -and $m -ne 0xC8 -and $m -ne 0xCC) {
            $r.height = [int](U16 $b ($pos + 5) $false); $r.width = [int](U16 $b ($pos + 7) $false)
            $r.pixelFormat = if ($b[$pos + 9] -eq 3) { "Format24bppRgb" } else { "Format8bppIndexed" }
            break
        }
        $pos += 2 + $len
    }
    return $r
}

function Inspect-Bmp([byte[]]$b) {
    $h = [int](U32 $b 22 $true); if ($h -gt 2147483647) { $h = [int]([long]$h - 4294967296L) }
    $bpp = U16 $b 28 $true
    return [ordered]@{
        width = [int](U32 $b 18 $true); height = [math]::Abs($h); frames = 1
        pixelFormat = if ($bpp -eq 24) { "Format24bppRgb" } elseif ($bpp -eq 32) { "Format32bppRgb" } else { Indexed $bpp }
        dpiX = [math]::Round((U32 $b 38 $true) * 0.0254, 2); dpiY = [math]::Round((U32 $b 42 $true) * 0.0254, 2)
    }
}

function Inspect-Gif([byte[]]$b) {
    # System.Drawing (used when the cases were captured) reports every GIF as 8 bpp indexed at 96 dpi.
    return [ordered]@{ width = [int](U16 $b 6 $true); height = [int](U16 $b 8 $true); pixelFormat = "Format8bppIndexed"; dpiX = 96; dpiY = 96; frames = 1 }
}

function Inspect-Pcx([byte[]]$b) {
    if ($b.Length -lt 128 -or $b[0] -ne 10 -or $b[2] -ne 1) { return $null }
    $bpp = $b[3]; $xmax = $b[8] + 256 * $b[9]; $ymax = $b[10] + 256 * $b[11]; $planes = $b[65]; $bpl = $b[66] + 256 * $b[67]
    $w = $xmax + 1; $h = $ymax + 1
    $need = $planes * $bpl * $h; $got = 0; $i = 128
    while ($got -lt $need -and $i -lt $b.Length) { $v = $b[$i]; $i++; if (($v -band 0xC0) -eq 0xC0) { $got += ($v -band 0x3F); $i++ } else { $got++ } }
    if ($got -ne $need) { return $null }
    $fmt = if ($planes -eq 3) { "Format24bppRgb" } elseif ($bpp -eq 8) { "Format8bppIndexed" } else { "Format1bppIndexed" }
    return [ordered]@{ width = $w; height = $h; pixelFormat = $fmt; dpiX = $b[12] + 256 * $b[13]; dpiY = $b[14] + 256 * $b[15]; frames = 1 }
}

function Inspect-File($path) {
    try {
        $b = [IO.File]::ReadAllBytes($path)
        if ($b.Length -lt 32) { return @{ unreadable = $true } }
        $r = if (($b[0] -eq 0x49 -and $b[1] -eq 0x49) -or ($b[0] -eq 0x4D -and $b[1] -eq 0x4D)) { Inspect-Tiff $b }
             elseif ($b[0] -eq 0x89 -and $b[1] -eq 0x50) { Inspect-Png $b }
             elseif ($b[0] -eq 0xFF -and $b[1] -eq 0xD8) { Inspect-Jpeg $b }
             elseif ($b[0] -eq 0x42 -and $b[1] -eq 0x4D) { Inspect-Bmp $b }
             elseif ($b[0] -eq 0x47 -and $b[1] -eq 0x49 -and $b[2] -eq 0x46) { Inspect-Gif $b }
             elseif ($b[0] -eq 10) { Inspect-Pcx $b }
             else { $null }
        if ($null -eq $r) { return @{ unreadable = $true } }
        return $r
    } catch { return @{ unreadable = $true } }
}

# ---------------------------------------------------------------- run the cases
$pass = 0; $fail = 0; $skipped = 0
$iniPath = Join-Path $exeDir "pdf2img.ini"
$iniBackup = if (Test-Path $iniPath) { [IO.File]::ReadAllText($iniPath) } else { $null }
try {
foreach ($case in $golden) {
    $name = $case.case
    if ($Only -and ($Only -notcontains $name)) { continue }
    $d = Join-Path $outRoot $name
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
    New-Item -ItemType Directory -Force $d | Out-Null
    $caseArgs = $case.args
    # which pdf does the case use?
    $pdfName = $null
    if ($caseArgs -match '\{D\}\\([^\\ "*?]+\.pdf)') { $pdfName = $Matches[1] }
    if ($name -eq "A16_wildcard") { $pdfName = "three.pdf" }   # the old run had only three.pdf in the directory
    if ($name -eq "C01_missing") { $pdfName = $null }         # the input is missing on purpose
    if ($name -eq "C18_overwrite") {
        $pdfName = "one.pdf"; $caseArgs = "-i {D}\one.pdf -o {D}\out.tif"
        [IO.File]::WriteAllText((Join-Path $d "out0001.tif"), "GARBAGE"); [IO.File]::WriteAllText((Join-Path $d "out0002.tif"), "GARBAGE2")
    }
    if ($name -eq "C19_unicode") {
        $pdfName = $null
        $ud = Join-Path $d $faDir
        New-Item -ItemType Directory -Force $ud | Out-Null
        Copy-Item (Join-Path $pdfDir "one.pdf") (Join-Path $ud "$faIn.pdf")
        $caseArgs = "-i `"{D}\$faDir\$faIn.pdf`" -o `"{D}\$faDir\$faOut.tif`""
    } elseif ($pdfName) {
        $src = Join-Path $pdfDir $pdfName
        if (-not (Test-Path $src)) { Write-Host "SKIP $name (missing $pdfName)"; $skipped++; continue }
        Copy-Item $src $d
    }
    # ini for this case
    $suffix = "%04d"; $pre = 0
    if ($name -eq "C09_ini_suffix") { $suffix = "_%03d" }
    if ($name -eq "C08_ini_pre") { $pre = 1 }
    [IO.File]::WriteAllText($iniPath, "[Options]`r`nIsPreProcessPDFFile=$pre`r`nAddFileNameSuffix=$suffix`r`n")

    $a = $caseArgs.Replace("{D}", $d)
    if (-not $onWindows) { $a = $a.Replace('\', '/') }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe; $psi.Arguments = $a; $psi.UseShellExecute = $false; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true; $psi.WorkingDirectory = $d
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8; $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [Diagnostics.Process]::Start($psi)
    $errTask = $p.StandardError.ReadToEndAsync()
    $stdout = $p.StandardOutput.ReadToEnd()
    if (-not $p.WaitForExit(180000)) { $p.Kill() }
    $p.WaitForExit()
    $stderr = $errTask.Result
    $sw.Stop()
    $exit = $p.ExitCode

    # expectations (file names use '/' as separator)
    $ov = $expectOverride[$name]
    $expExit = if ($ov) { $ov.exit } else { $case.exitCode }
    $expFiles = @{}
    $refCase = $case
    if ($ov -and $ov.like) { $refCase = $golden | Where-Object { $_.case -eq $ov.like } }
    if ($ov -and $null -ne $ov.files) {
        $i = 0
        foreach ($fn in $ov.files) {
            $refFile = if ($refCase.files.Count -gt $i) { $refCase.files[$i] } else { $null }
            if ($name -eq "C18_overwrite" -and $fn -eq "out0002.tif") { $expFiles[$fn] = [ordered]@{ garbage = $true } }
            elseif ($refFile) { $expFiles[$fn] = Normalize-Expected $refFile }
            else { $expFiles[$fn] = [ordered]@{} }
            $i++
        }
    } else {
        foreach ($f in $case.files) { $expFiles[$f.name.Replace('\', '/')] = Normalize-Expected $f }
    }

    $problems = @()
    if ($exit -ne $expExit) { $problems += "exit code $exit (expected $expExit)" }
    $actual = @{}
    Get-ChildItem $d -File -Recurse | Where-Object { $_.Extension -ne ".pdf" -and $_.Name -notlike "*.tmp" } |
        ForEach-Object { $actual[$_.FullName.Substring($d.Length + 1).Replace('\', '/')] = $_.FullName }
    foreach ($fn in $expFiles.Keys) { if (-not $actual.ContainsKey($fn)) { $problems += "missing output $fn" } }
    foreach ($fn in $actual.Keys) { if (-not $expFiles.ContainsKey($fn)) { $problems += "unexpected output $fn" } }
    foreach ($fn in $expFiles.Keys) {
        if (-not $actual.ContainsKey($fn)) { continue }
        $exp = $expFiles[$fn]
        if ($exp.Contains("garbage")) { continue }
        $act = Inspect-File $actual[$fn]
        if ($act.unreadable) { if ($exp.Count -gt 0) { $problems += "$fn unreadable" }; continue }
        foreach ($k in @("width", "height", "frames", "tiff_compression", "tiff_photometric", "tiff_bitsPerSample", "tiff_samplesPerPixel")) {
            if ($exp.Contains($k) -and $act.Contains($k) -and $exp[$k] -ne $act[$k]) { $problems += "$fn ${k}: $($act[$k]) (expected $($exp[$k]))" }
            elseif ($exp.Contains($k) -and -not $act.Contains($k)) { $problems += "$fn ${k}: missing (expected $($exp[$k]))" }
        }
        if ($exp.Contains("pixelFormat") -and $act.Contains("pixelFormat") -and $exp.pixelFormat -ne $act.pixelFormat) {
            # accept 8bpp gray JPEG where the old tool wrote a 24bpp JPEG of gray pixels
            $okDiff = ($fn -like "*.jpg" -and $exp.pixelFormat -eq "Format24bppRgb" -and $act.pixelFormat -eq "Format8bppIndexed")
            if (-not $okDiff) { $problems += "$fn pixelFormat: $($act.pixelFormat) (expected $($exp.pixelFormat))" }
        }
        foreach ($k in @("dpiX", "dpiY")) {
            if ($exp.Contains($k) -and $act.Contains($k) -and [math]::Abs($exp[$k] - $act[$k]) -gt 0.6) { $problems += "$fn ${k}: $($act[$k]) (expected $($exp[$k]))" }
        }
    }
    if ($problems.Count -eq 0) { $pass++; if (-not $Quiet) { Write-Host ("PASS {0,-22} {1,6} ms  {2}" -f $name, $sw.ElapsedMilliseconds, ($actual.Keys -join ", ")) } }
    else {
        $fail++
        Write-Host ("FAIL {0,-22} {1,6} ms" -f $name, $sw.ElapsedMilliseconds) -ForegroundColor Red
        foreach ($pr in $problems) { Write-Host "     - $pr" -ForegroundColor Red }
        if ($stderr) { Write-Host ("     stderr: " + ($stderr.Trim() -replace "`n", "`n             ")) -ForegroundColor DarkGray }
    }
}
} finally {
    if ($null -ne $iniBackup) { [IO.File]::WriteAllText($iniPath, $iniBackup) } elseif (Test-Path $iniPath) { Remove-Item $iniPath }
}
Write-Host ""
Write-Host "golden: $pass passed, $fail failed, $skipped skipped"
exit $(if ($fail -gt 0) { 1 } else { 0 })
