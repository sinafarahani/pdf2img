# pdf2img

Convert PDF pages to images from the command line: TIFF (single or multi-page, LZW, JPEG, PackBits, G3/G4 fax),
JPEG, PNG, BMP, GIF, PCX and TGA. Runs on Windows, Linux and macOS and renders with
[PDFium](https://pdfium.googlesource.com/pdfium/), the PDF engine of Chrome.

pdf2img is also a **drop-in replacement for the command-line version of VeryPDF "PDF To Image Converter" 2.1**
(`pdf2img.exe`, 2006). It accepts the same switches and produces the same file names, pixel sizes and image formats,
so software that calls the old program keeps working after you swap the binary, without the freezes, pop-up
dialogs, trial limits and watermarks. pdf2img is an independent project, not affiliated with VeryPDF.

## Features
- **Fast**: pages are rendered in parallel worker processes, one per CPU core (up to 8 by default).
- **Never hangs, never shows a dialog**: every page has a time limit (a page that exceeds it fails without stopping
  the others), the whole run has a time limit, and each worker has a memory limit.
- **Robust**: damaged PDFs are rebuilt by PDFium or, if that is not enough, repaired with qpdf first; work lost to a
  crashed worker is retried once.
- Password-protected PDFs (`-upw` / `-opw`), Unicode file names, long paths on Windows.
- Useful exit codes, silent on success, optional errors-only log file.
- Nothing to install: one executable plus the PDFium library.

## Download
Prebuilt archives are on the [Releases page](../../releases/latest):

| Platform | Archive |
|---|---|
| Windows 10 / 11, x64 | `pdf2img-<version>-windows-x64.zip` |
| Linux x64 (glibc 2.35 or newer: Ubuntu 22.04+, Debian 12+, Fedora 36+, RHEL 9+) | `pdf2img-<version>-linux-x64.tar.gz` |
| Linux arm64 (same) | `pdf2img-<version>-linux-arm64.tar.gz` |
| macOS 11 or newer, Apple silicon | `pdf2img-<version>-macos-arm64.tar.gz` |
| macOS 11 or newer, Intel | `pdf2img-<version>-macos-x64.tar.gz` |

Unpack the archive anywhere and keep the executable and the PDFium library (`pdfium.dll`, `libpdfium.so` or
`libpdfium.dylib`) in the same folder. Nothing else is needed: no Ghostscript, no runtime to install.

On macOS the binaries are not notarized. If macOS refuses to run them, remove the download quarantine:
`xattr -dr com.apple.quarantine pdf2img-<version>-macos-arm64`.

## Usage
```
pdf2img [options] -i <PDF file> [-o <output file>]
```

| Option | Meaning |
|---|---|
| `-i <file>` | Input PDF (required). A wildcard converts several files: `-i "C:\in\*.pdf"`. On Linux/macOS put the wildcard in quotes so that the shell does not expand it. |
| `-o <file>` | Output file. The extension selects the format: `tif` `tiff` `jpg` `jpeg` `png` `bmp` `gif` `pcx` `tga`. Default: the input's name with `.tif`, in the input's folder. With a wildcard input use a wildcard output (`-o "C:\out\*.png"`) or an existing folder. |
| `-r <dpi>` | Resolution: `-r 300` or `-r 200x300`. Default 101 (the old tool's default). |
| `-f <n>`, `-l <n>` | First and last page to convert (1-based). |
| `-b <bits>` | `1` black and white, `4` 16 colours, `8` 256 colours, `24` true colour (default). |
| `-g` | Together with `-b 8`: 8-bit grayscale instead of a colour palette. |
| `-c <method>` | TIFF compression: `none`, `lzw`, `jpeg`, `packbits` (default), `g3`, `g4`, `ClassF` (fax: 1728 px wide, 204 x 98 dpi), `ClassF196` (204 x 196 dpi). `g3`, `g4` and `ClassF*` produce black-and-white images. |
| `-m` | All pages in one multi-page TIFF. |
| `-q <n>` | JPEG quality 1-100 (default 90). |
| `-d` | `-o` names a folder; each input goes to `<folder>/<input name>/<input name>0001.tif` ... |
| `-?`, `-h` | Help (`-h` also lists the switches below). |

Additional switches (not in the old tool):

| Option | Meaning |
|---|---|
| `-upw <password>` | User password of an encrypted PDF. |
| `-opw <password>` | Owner password of an encrypted PDF. |
| `-timeout <seconds>` | Time limit per page (default 120). |
| `-totaltimeout <seconds>` | Time limit for the whole run (default 1800). |
| `-workers <n>` | Number of parallel render processes (default: CPU cores, at most 8). |
| `-v` | Report every page and step on stderr (and in the log file). |
| `-log <file>` | Append errors and warnings to a file. |
| `-version` | Print the version. |

### Examples
```
pdf2img -i report.pdf -o page.png                   # page.png (1 page) or page0001.png, page0002.png, ...
pdf2img -c lzw -r 300 -i report.pdf -o scan.tif     # 300 dpi LZW TIFF, one file per page: scan0001.tif, ...
pdf2img -m -c g4 -r 200 -i report.pdf -o fax.tif    # one multi-page black-and-white TIFF
pdf2img -f 2 -l 5 -q 80 -i report.pdf -o page.jpg   # pages 2 to 5 as JPEG
pdf2img -i "invoices/*.pdf" -o "images/*.png"       # every PDF in a folder
pdf2img -upw secret -i locked.pdf -o page.png       # encrypted PDF
```

### Output file names
- A page number is appended to the name, formatted as `%04d` by default: `page0001.png`.
- TIFF is always numbered (`scan0001.tif`, even for a one-page PDF), except with `-m` (`scan.tif`).
- Other formats are numbered only when more than one page is written (`page.png` for a single page).
- The number is the real page number: `-f 3 -l 4` writes `page0003.png` and `page0004.png`.
- `-f` beyond the last page converts the whole document, `-l` beyond it stops at the last page, and `-l` smaller than
  `-f` converts only page `-f` (the old tool's rules).
- Existing files are overwritten; missing output folders are created.
- In wildcard mode each output is named after its input, and a line `Converting <file>......` is printed per file.

### Exit codes
| Code | Meaning |
|---|---|
| 0 | all requested pages were written |
| 1 | bad or missing arguments, or help was shown |
| 2 | input not found, not a PDF, or no pages |
| 3 | one or more pages could not be rendered |
| 4 | an output file could not be written |
| 5 | a page or the whole run exceeded its time limit |
| 6 | unknown or missing output extension (also WMF/EMF, which are not supported) |
| 7 | the PDF needs a password (`-upw`/`-opw`) or the password is wrong |
| 9 | internal failure (for example a worker kept crashing, or the PDFium library is missing) |

With several failures the most severe code is returned. Every problem is printed on stderr as
`pdf2img: error: ...`; nothing is printed on success.

## Settings
Settings are optional. Put a `pdf2img.ini` next to the executable (a commented `pdf2img.ini.example` is included in
the archives) or set the matching environment variable, which takes precedence:

| `[Options]` key | Environment variable | Default | Meaning |
|---|---|---|---|
| `AddFileNameSuffix` | `PDF2IMG_SUFFIX` | `%04d` | page-number format (exactly one `%d`) |
| `DefaultDPI` | `PDF2IMG_DEFAULT_DPI` | 101 | resolution when `-r` is not given |
| `JpegQuality` | `PDF2IMG_JPEG_QUALITY` | 90 | JPEG quality when `-q` is not given |
| `PageTimeoutSec` | `PDF2IMG_PAGE_TIMEOUT` | 120 | time limit per page |
| `TotalTimeoutSec` | `PDF2IMG_TOTAL_TIMEOUT` | 1800 | time limit per run (0 = none) |
| `Workers` | `PDF2IMG_WORKERS` | 0 | parallel render processes (0 = CPU cores, max 8) |
| `WorkerMemoryLimitMB` | `PDF2IMG_WORKER_MEMORY_MB` | 2048 | memory limit per render process |
| `MaxBitmapMB` | `PDF2IMG_MAX_BITMAP_MB` | 1536 | larger pages are rendered at `DefaultDPI` instead |
| `LzwPredictor` | `PDF2IMG_LZW_PREDICTOR` | 1 | horizontal predictor for LZW TIFF (smaller files) |
| `Antialias` | `PDF2IMG_ANTIALIAS` | 1 | anti-aliasing of text and graphics |
| `Verbose` | `PDF2IMG_VERBOSE` | 0 | 1 = report every page and step |
| `LogFile` | `PDF2IMG_LOG` | (none) | append errors and warnings to this file |
| `IsPreProcessPDFFile` | | | accepted for compatibility with the old tool, ignored |

**Log file.** Only errors and warnings are written (everything with `Verbose=1` or `-v`), each record with a time
stamp and process id, preceded once per run by the run's command line with `-upw`/`-opw` values replaced by `***`.
A successful run writes nothing, so the log can stay enabled. If the file cannot be written, records go to
`pdf2img-errors.log` next to the executable, or in the temp folder.

## Replacing VeryPDF PDF To Image Converter 2.1
1. Rename `pdf2img.exe` to the file name your software starts (for example `pdf2img.exe` or `0.exe`) and put
   `pdfium.dll` in the same folder. Without the DLL every run fails with exit code 9 and an error that names it.
2. An existing `pdf2img.ini` keeps working (`AddFileNameSuffix`; `IsPreProcessPDFFile` is ignored).
3. What changes for the calling software: failures now return non-zero exit codes (the old tool returned 0 for
   almost everything), an unknown output extension is an error instead of empty files, WMF/EMF are not supported,
   and password-protected PDFs need `-upw`. The complete list is in
   [docs/DESIGN.md](docs/DESIGN.md#deliberate-differences-from-the-old-tool); the observed behaviour of the old tool
   is documented in [docs/OLD_TOOL_BEHAVIOR.md](docs/OLD_TOOL_BEHAVIOR.md).

## How it works
`pdf2img` starts copies of itself as render workers (`pdf2img --worker`) and hands them one page at a time over a
pipe. Workers render with PDFium, convert the page to the requested depth and palette, and encode it with libtiff,
libjpeg-turbo, libpng or giflib. The supervisor enforces the time and memory limits, restarts workers that crash or
hang, and combines the results into the exit code. Details: [docs/DESIGN.md](docs/DESIGN.md).

## Building from source
You need CMake 3.25+, Ninja, a C++23 compiler (Visual Studio 2022 or newer, GCC 11+, Clang 15+ / Xcode 15+) and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` pointing to it. vcpkg builds qpdf, libtiff,
libjpeg-turbo, libpng, zlib and giflib; the prebuilt PDFium library is downloaded when CMake configures.

**Windows** (in a "Developer PowerShell for VS"):
```
git clone https://github.com/microsoft/vcpkg C:\vcpkg; C:\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset release
cmake --build --preset release
ctest --preset release
```
The executable is `build\release\pdf2img.exe`. `scripts\build.ps1` does the same from a plain PowerShell. The Visual
Studio project `new.vcxproj` (x64, uses vcpkg's manifest mode) builds `x64\Release\pdf2img.exe`.

**Linux** (Debian/Ubuntu package names):
```
sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config nasm
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
```

**macOS**:
```
brew install cmake ninja nasm pkg-config
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
cmake --preset macos-release && cmake --build --preset macos-release && ctest --preset macos-release
```

The first build takes a while because vcpkg compiles the libraries. The result is `build/<preset>/pdf2img` with the
PDFium library next to it.

### Tests
- `ctest --preset <preset>`: unit tests (naming rules, command line, conversions, encoders, platform helpers).
- `pwsh tests/run_golden.ps1 -Exe <pdf2img>`: replays the command lines that were run against the old tool and
  compares exit codes, file names, pixel sizes, bit depths, resolutions and TIFF tags.
- `pwsh tests/run_robustness.ps1 -Exe <pdf2img>`: time limits, crashing and slow workers, retries, passwords, the
  memory limit, damaged PDFs, the log file and cleanup.

Every push is built and tested on all five platforms by GitHub Actions; pushing a tag `v*` publishes a release with
the archives.

## License
pdf2img is released under the [MIT license](LICENSE). The third-party components (PDFium, qpdf, libtiff,
libjpeg-turbo, libpng, zlib, giflib) and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
