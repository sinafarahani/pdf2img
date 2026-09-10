# Robustness checks against a pdf2img build: page and run time limits, crashing workers and their retries, slow
# workers, passwords, the worker memory limit, damaged PDFs, the errors-only log file, and cleanup (no leftover
# processes or temp files). Uses the PDF2IMG_TEST_* hooks of the worker. Runs on Windows, Linux and macOS.
# Usage: pwsh tests/run_robustness.ps1 [-Exe build/linux-release/pdf2img]
param([string]$Exe = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Exe) {
    $Exe = @("build/release/pdf2img.exe", "build/linux-release/pdf2img", "build/macos-release/pdf2img") |
        ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Exe) { throw "no build found; pass -Exe" }
}
$Exe = (Resolve-Path $Exe).Path
$pdfs = Join-Path $PSScriptRoot "golden/pdfs"
$T = Join-Path $PSScriptRoot "out/robustness"
if (Test-Path $T) { Remove-Item $T -Recurse -Force }
New-Item -ItemType Directory -Force $T | Out-Null
$script:fail = 0

function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host "PASS  $name   $detail" } else { $script:fail++; Write-Host "FAIL  $name   $detail" -ForegroundColor Red }
}
function Quote([string]$a) { if ($a -match '[\s"]' -or $a -eq "") { '"' + $a + '"' } else { $a } }
function Start-Exe([string[]]$argv, [hashtable]$envVars = @{}) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ($argv | ForEach-Object { Quote $_ }) -join ' '
    $psi.UseShellExecute = $false; $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
    foreach ($k in $envVars.Keys) { $psi.EnvironmentVariables[$k] = [string]$envVars[$k] }
    $p = [Diagnostics.Process]::Start($psi)
    return [pscustomobject]@{ p = $p; err = $p.StandardError.ReadToEndAsync(); out = $p.StandardOutput.ReadToEndAsync() }
}
function Run([string[]]$argv, [hashtable]$envVars = @{}) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $h = Start-Exe $argv $envVars
    if (-not $h.p.WaitForExit(120000)) { $h.p.Kill(); $h.p.WaitForExit() }
    $sw.Stop()
    return [pscustomobject]@{ code = $h.p.ExitCode; ms = $sw.ElapsedMilliseconds; err = $h.err.Result; out = $h.out.Result }
}
function Text($path) { if (Test-Path $path) { [IO.File]::ReadAllText($path) } else { "" } }
function Count($dir, $filter) { @(Get-ChildItem $dir -Filter $filter -ErrorAction SilentlyContinue).Count }
function Workers() { @(Get-Process -Name pdf2img -ErrorAction SilentlyContinue).Count }
function In([string]$name) { Join-Path $T $name }

Write-Host "exe: $Exe"
$three = Join-Path $pdfs "three.pdf"

# 1. a page that never finishes: only that page fails (exit 5), the others are written
$r = Run @('-timeout', '1', '-i', $three, '-o', (In "t1/x.png")) @{ PDF2IMG_TEST_HANG_PAGE = 2 }
Check "01 hanging page: exit 5, pages 1 and 3 written" ($r.code -eq 5 -and (Test-Path (In "t1/x0001.png")) -and (Test-Path (In "t1/x0003.png")) -and -not (Test-Path (In "t1/x0002.png"))) "code=$($r.code) $($r.ms) ms"

# 2. a page that always crashes its worker: retried once, then exit 9; the other pages are written
$r = Run @('-i', $three, '-o', (In "t2/x.png")) @{ PDF2IMG_TEST_CRASH_PAGE = 2 }
Check "02 page crashing every worker: exit 9, other pages written" ($r.code -eq 9 -and (Count (In "t2") "*.png") -eq 2 -and $r.err -match 'also failed on a retry') "code=$($r.code)"

# 3. a worker that crashes once while rendering: the page is retried on another worker
$r = Run @('-i', $three, '-o', (In "t3/x.png")) @{ PDF2IMG_TEST_CRASH_ONCE_PAGE = 2; PDF2IMG_TEST_CRASH_ONCE_PAGE_MARKER = (In "t3.marker") }
Check "03 worker crashes once on a page: retried, exit 0, 3 pages" ($r.code -eq 0 -and (Test-Path (In "t3.marker")) -and (Count (In "t3") "*.png") -eq 3) "code=$($r.code)"

# 4. a worker that crashes once while opening the PDF
$r = Run @('-i', $three, '-o', (In "t4/x.png")) @{ PDF2IMG_TEST_CRASH_ONCE_OPEN_MARKER = (In "t4.marker") }
Check "04 worker crashes once while opening: retried, exit 0, 3 pages" ($r.code -eq 0 -and (Test-Path (In "t4.marker")) -and (Count (In "t4") "*.png") -eq 3) "code=$($r.code)"

# 5. every worker dies right after reporting a page
$r = Run @('-workers', '2', '-i', $three, '-o', (In "t5/x.png")) @{ PDF2IMG_TEST_CRASH_AFTER_EVERY_PAGE = 1 }
Check "05 workers die after every page: exit 0, 3 pages" ($r.code -eq 0 -and (Count (In "t5") "*.png") -eq 3) "code=$($r.code)"

# 6. multi-page TIFF with one crash inside: the whole TIFF is retried
$r = Run @('-m', '-i', $three, '-o', (In "t6/m.tif")) @{ PDF2IMG_TEST_CRASH_ONCE_PAGE = 2; PDF2IMG_TEST_CRASH_ONCE_PAGE_MARKER = (In "t6.marker") }
Check "06 crash inside a multi-page TIFF: retried, exit 0" ($r.code -eq 0 -and (Test-Path (In "t6/m.tif")) -and (Test-Path (In "t6.marker"))) "code=$($r.code)"

# 7. workers that are slow to quit do not delay the end of the run
$base = Run @('-i', $three, '-o', (In "t7a/x.png"))
$slow = Run @('-i', $three, '-o', (In "t7b/x.png")) @{ PDF2IMG_TEST_SLOW_EXIT_MS = 5000 }
Check "07 slow-to-quit workers: run not delayed" ($slow.code -eq 0 -and $slow.ms -lt ($base.ms + 3000)) "baseline $($base.ms) ms, slow $($slow.ms) ms"

# 8. time limit for the whole run
$r = Run @('-totaltimeout', '2', '-timeout', '60', '-i', $three, '-o', (In "t8/x.png")) @{ PDF2IMG_TEST_HANG_PAGE = 1 }
Check "08 total time limit: exit 5 within seconds" ($r.code -eq 5 -and $r.ms -lt 15000 -and $r.err -match 'not completed') "code=$($r.code) $($r.ms) ms"

# 9. passwords
$enc = Join-Path $pdfs "enc.pdf"
$r1 = Run @('-i', $enc, '-o', (In "t9/a.png"))
$r2 = Run @('-upw', 'secret', '-i', $enc, '-o', (In "t9/b.png"))
$r3 = Run @('-upw', 'wrong', '-i', $enc, '-o', (In "t9/c.png"))
$r4 = Run @('-i', (Join-Path $pdfs "ownerpw.pdf"), '-o', (In "t9/d.png"))
Check "09 passwords: none -> 7, right -> 0, wrong -> 7, owner-only -> 0" ($r1.code -eq 7 -and $r2.code -eq 0 -and $r3.code -eq 7 -and $r4.code -eq 0 -and (Test-Path (In "t9/b.png"))) "$($r1.code) $($r2.code) $($r3.code) $($r4.code)"

# 10. log file: errors only, passwords masked
$log = In "t10.log"
$ok = Run @('-log', $log, '-i', $three, '-o', (In "t10/ok.png"))
$afterOk = Text $log
$bad = Run @('-log', $log, '-upw', 'Wr0ngPass', '-i', $enc, '-o', (In "t10/bad.png"))
$txt = Text $log
Check "10 log file: nothing on success, error with masked password" ($ok.code -eq 0 -and $afterOk.Length -eq 0 -and $bad.code -eq 7 -and $txt -match '-upw \*\*\*' -and $txt -notmatch 'Wr0ngPass' -and $txt -match 'error: ') ($txt.Trim() -replace "`r?`n", ' | ')

# 11. worker memory limit
$r = Run @('-i', (Join-Path $pdfs "one.pdf"), '-o', (In "t11/x.png")) @{ PDF2IMG_WORKER_MEMORY_MB = 256; PDF2IMG_TEST_ALLOC_MB = 700 }
Check "11 worker over the memory limit: page fails with exit 3" ($r.code -eq 3 -and $r.err -match 'memory limit|allocation') "code=$($r.code) $($r.err.Trim())"

# 12. damaged PDF (no header): repaired with qpdf, converted, warning
$r = Run @('-i', (Join-Path $pdfs "damaged.pdf"), '-o', (In "t12/x.png"))
Check "12 damaged PDF: repaired, exit 0, warning" ($r.code -eq 0 -and (Count (In "t12") "*.png") -eq 3 -and $r.err -match 'repaired with qpdf') "code=$($r.code)"

# 13. argument and input errors
$u = Run @('-z', '-i', $three)
$m = Run @('-i', (In "missing.pdf"), '-o', (In "t13/x.png"))
$x = Run @('-i', $three, '-o', (In "t13/x.xyz"))
$v = Run @('-version')
$c = Run @('-i', (Join-Path $pdfs "corrupt.pdf"), '-o', (In "t13/c.png"))
Check "13 errors: bad option 1, missing input 2, unknown format 6, corrupt 2, -version 0" ($u.code -eq 1 -and $m.code -eq 2 -and $x.code -eq 6 -and $c.code -eq 2 -and $v.code -eq 0 -and $v.out -match 'pdf2img \d') "$($u.code) $($m.code) $($x.code) $($c.code) $($v.code)"

# 14. the supervisor is killed while a worker hangs: the workers go away too
$h = Start-Exe @('-timeout', '60', '-i', $three, '-o', (In "t14/x.png")) @{ PDF2IMG_TEST_HANG_PAGE = 1 }
Start-Sleep -Milliseconds 2000
$before = Workers
$h.p.Kill(); $h.p.WaitForExit()
$sw = [Diagnostics.Stopwatch]::StartNew()
while ((Workers) -gt 0 -and $sw.ElapsedMilliseconds -lt 8000) { Start-Sleep -Milliseconds 200 }
Check "14 supervisor killed: its workers exit as well" ($before -ge 2 -and (Workers) -eq 0) "processes before kill: $before, gone after $($sw.ElapsedMilliseconds) ms"

Start-Sleep -Milliseconds 500
Check "15 no leftover worker processes" ((Workers) -eq 0)
$tmpDir = [IO.Path]::GetTempPath()
Check "16 no temp files left behind" (@(Get-ChildItem $T -Recurse -Filter *.tmp).Count -eq 0 -and @(Get-ChildItem $tmpDir -Filter 'pdf2img_*.pdf' -ErrorAction SilentlyContinue).Count -eq 0)

Write-Host ""
Write-Host "robustness: $script:fail failure(s)"
exit $script:fail
