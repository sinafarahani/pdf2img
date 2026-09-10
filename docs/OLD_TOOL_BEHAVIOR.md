# Behaviour of the VeryPDF command line that pdf2img reproduces

pdf2img replaces the command-line program of VeryPDF "PDF To Image Converter" v2.1 (`pdf2img.exe`, December 2006,
32-bit Windows). This page records how that program behaves, as observed by running it (black-box) on the test files
in `tests/golden/pdfs`. The captured results are in `tests/golden/golden.json` and are replayed against pdf2img by
`tests/run_golden.ps1`. Where pdf2img deliberately behaves differently, the difference is listed in
[DESIGN.md](DESIGN.md#deliberate-differences-from-the-old-tool).

## Command line
getopt-style switches (`i:o:$mgr:f:l:c:q:b:d?`): `-i -o -r -f -l -c -q -b` take a value (`-r 300` or `-r300`), `-m -g -d
-$ -?` are flags and can be clustered (`-mg`). Switches may appear in any order, before or after `-i`/`-o`; letters are
case-sensitive. `-d` and `-$` are undocumented.

| Case | Old tool |
|---|---|
| no arguments, `-?`, unknown switch (`-z`) | prints its usage text, waits for a key press, **exit 1**, no files |
| input given without `-i` (`in.pdf -o out.tif`) | rejected like an unknown argument: usage, exit 1 (one example in its own help text is wrong) |
| `-i in.pdf -o out.tif`, 17 pages | `out0001.tif` ... `out0017.tif`, exit 0 |
| 1-page PDF to `.tif`/`.tiff` | `out0001.tif`: TIFF output is **always** numbered unless `-m` is given |
| 1-page PDF to `.jpg`, `.png`, ... | `out.jpg`: other formats are numbered **only when more than one page** is written |
| `-f 2 -l 3` | `out0002.tif`, `out0003.tif`: the number is the real page number |
| `-f 5` on a 3-page PDF | the range is ignored, all pages are converted |
| `-f 3 -l 2` | only page 3 |
| `-m` with `.tif` | one multi-page `out.tif`, no number |
| `-m` with any other format | `-m` is ignored |
| no `-o` | `<input folder>\<input name>0001.tif` ... |
| `-o out` (no extension), `-o out.xyz` | **0-byte files** `out0001`, `out0001.xyz`, ..., exit 0 |
| `-o` into a folder that does not exist | nothing written, exit 0 |
| `-i dir\*.pdf -o dir\*.jpg` | every matching PDF converted; output name = input name + number + extension of `-o`, in the folder of `-o`; prints `Converting <path>......` for each file |
| missing input, corrupt PDF, password-protected PDF | nothing written, **exit 0**, no message |
| existing output files | overwritten; older files from a previous run with more pages are left in place |
| names with non-ANSI characters | only work when the Windows ANSI code page can represent them |
| `-d` | `-o` names a folder; output goes to `<folder>\<input name>\<input name>0001.tif` |
| `-$` | no visible effect on the conversion |
| `pdf2img.ini` next to the exe: `[Options] AddFileNameSuffix=_%03d` | `out_001.tif` ...: a printf format applied to the page number (default `%04d`) |
| `pdf2img.ini`: `IsPreProcessPDFFile=1` | an extra pre-processing pass (needs Ghostscript) |
| exit codes | 0 for everything that got past argument parsing, even total failure; 1 for argument errors |
| console output | nothing on success (except the `Converting` lines in wildcard mode) |

Things that made it hang in unattended use: modal dialog boxes (registration reminder, "Ghostscript not found",
"page too large, use the default resolution?"), the key-press wait after the usage text, runtime error boxes, and
renderer processes that never finish on some malformed PDFs.

## Image output
- Default resolution **101 dpi** (a Letter page becomes 858 x 1111 px; `-r 300` gives 2550 x 3300, `-r 200x100` gives
  1700 x 1100). Pixel size = `(int)(points * dpi / 72 + 0.5)` per axis (858.5 -> 858, 420.83 -> 421). The crop box is
  used. The resolution is written into the files (TIFF X/YResolution, JPEG JFIF density, PNG pHYs, BMP pixels per metre).
- Rendering is anti-aliased. Default depth 24-bit RGB.
- `-b 1`: black and white by a luminance threshold (no dithering), TIFF photometric 0 (WhiteIsZero).
  `-b 4`: 16-colour palette (the standard VGA palette). `-b 8`: 256-colour palette (the Windows halftone palette),
  photometric 3. `-b 8 -g`: 8-bit grayscale, photometric 1. `-g` without `-b 8` has no effect. Palettes are fixed,
  nearest colour, no dithering.
- TIFF: little-endian, one strip per image, default compression **PackBits** (32773) for every depth. `-c none` = 1,
  `lzw` = 5, `jpeg` = 7, `packbits` = 32773, `g3` = 3, `g4` = 4. G3, G4 and ClassF force 1-bit (`-b 24 -c g4` gives a
  1-bit G4 file). FillOrder=2 on every file.
- `-c ClassF`: 1-bit G4 fax, 204 x 98 dpi, scaled to the fax width of **1728 px** keeping the aspect ratio
  (Letter -> 1728 x 1074). `-c ClassF196`: the same at 204 x 196 dpi (Letter -> 1728 x 2148). `-r` is ignored.
- JPEG: baseline JFIF, 4:2:0, quality 90 by default (`-q` = libjpeg quality), always 3 components (also for gray).
- PNG 24-bit with pHYs; GIF 8-bit palette; BMP 24-bit uncompressed; PCX 24-bit (3 planes, RLE); TGA 24-bit.
- A 60 x 40 inch page is rendered at 6060 x 4040 px (101 dpi) without asking.
- WMF/EMF: vector output, not raster; name without a number (`out.wmf`).
- Unregistered copies draw a text watermark over every page.
