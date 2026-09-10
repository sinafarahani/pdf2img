# pdf2img design

## Goals
- The command line and output naming of VeryPDF PDF To Image Converter 2.1 (`pdf2img.exe`), so that programs written
  for it keep working unchanged ([OLD_TOOL_BEHAVIOR.md](OLD_TOOL_BEHAVIOR.md)).
- Never hang and never show a dialog, whatever the input: unattended batch use.
- Fast on multi-core machines; Windows, Linux and macOS; no installation.

## Why PDFium (and qpdf)
qpdf is a PDF *structure* library (objects, streams, encryption, cross-reference repair); it cannot draw pages. The
renderer is **PDFium** (BSD-3, the engine of Chrome): robust on malformed files, progressive rendering that can be
paused, and prebuilt shared libraries for every platform from
[pdfium-binaries](https://github.com/bblanchon/pdfium-binaries). MuPDF (AGPL), Poppler (GPL) and Ghostscript (AGPL)
were ruled out by their licenses. qpdf is kept as the repair layer: when PDFium cannot open a file, qpdf rewrites it
(recovering the cross-reference table, decrypting with the given password) into a temporary copy and PDFium tries again.

## Process model
PDFium is single-threaded and some malformed files make it loop inside calls that cannot be interrupted
(`FPDF_LoadPage`, parsing). The only reliable way to guarantee progress is to render in separate processes:

```
pdf2img (supervisor)  --stdin/stdout pipes-->  pdf2img --worker  (x N, one PDFium instance each)
```

- The **supervisor** parses the command line, resolves inputs (wildcards) and output names, and keeps a queue of work
  units: open a PDF, render one page to one file, or render a page range into one multi-page TIFF. It starts up to N
  workers (default: CPU cores, at most 8), dispatches units (preferring a worker that already has the document open),
  collects results and computes the exit code (the most severe failure wins).
- **Workers** are the same executable started with `--worker`. They render with PDFium into the output image buffer,
  convert it (threshold, fixed palettes, fax resampling) and encode it with libtiff / libjpeg-turbo / libpng / giflib
  (BMP, PCX and TGA are written directly). Every file is written to `<name>.<pid>.tmp` and renamed into place, so a
  killed worker never leaves a truncated image behind.
- The protocol is one line per message, TAB-separated UTF-8 fields (see `src/ipc.h`): `SPEC`/`SPECEND` (settings),
  `OPEN`, `PAGE`, `MULTI`, `QUIT` to the worker; `READY`, `FATAL`, `BEGIN`, `OPENED`/`OPENFAIL`, `PAGEOK`/`PAGEFAIL`,
  `PROGRESS`, `MULTIOK`/`MULTIFAIL` back.

### Limits and failure handling
- **Per page**: PDFium's pause callback stops rendering cooperatively at the page time limit (default 120 s); if the
  worker does not report back within the limit plus a grace period (5-15 s) it is killed and the page fails with exit
  code 5. The other pages continue on other workers.
- **Whole run**: default 30 minutes; unfinished inputs are named in the error output.
- **Memory**: Windows runs each worker in a Job Object with a process memory limit (default 2 GB) that also kills the
  worker when the supervisor ends. Linux/macOS have no Job Objects: the supervisor reads the private memory of busy
  workers (resident minus file-backed pages from `/proc/<pid>/statm`; the physical footprint from `proc_pid_rusage`,
  so the memory-mapped input PDF is not counted, as on Windows) about ten times a second and kills a worker that
  exceeds the limit; workers exit on their own when the supervisor disappears (they watch their parent process).
- **Crashes**: work lost to a worker that dies is retried once on another worker. Work sent to a worker that died
  before acknowledging it (`BEGIN`) was never attempted and is resubmitted without using up the retry.
- **Oversized pages**: if a page bitmap would exceed 1.5 GB (or 65000 px per side) the page is rendered at the
  default resolution instead, with a warning (the old tool asked in a dialog).
- **No dialogs**: Windows error mode and CRT report hooks are set so that nothing ever opens a window; stdin is never
  read by the supervisor.
- **Shutdown**: at the end all workers are asked to quit at once, get one shared 0.5 s grace period, and stragglers
  are killed together.

## Rendering and conversion
- Pixel size per axis: `(int)(points * dpi / 72 + 0.5)`, the old tool's rounding; the crop box is rendered.
- Colour output renders straight to BGR24; grayscale and 1-bit output render to 8-bit gray (a quarter of the memory).
  AcroForm fields are drawn through a form-fill environment (`FPDF_FFLDraw`).
- 1-bit: luminance threshold at 128, no dithering. 4-bit and 8-bit: nearest colour in the fixed VGA-16 and Windows
  halftone-256 palettes (a lazily filled 24-bit lookup table). ClassF: gray, scaled to 1728 px wide, then threshold.
- The input PDF is memory-mapped read-only, so all workers share one copy in the page cache.

## Platform layer
Everything outside `common.cpp`, `fsutil.cpp`, `child_process.cpp`, `log.cpp`, `config.cpp` and the worker's pipe I/O is
portable C++. Paths are `std::wstring` internally (UTF-16 on Windows, UTF-32 elsewhere) and UTF-8 on the wire and on
Linux/macOS file systems; bytes that are not valid UTF-8 survive the round trip (they are carried as U+DC80..U+DCFF).
On Windows, `pdfium.dll` is delay-loaded so that a missing DLL is reported with a clear message and exit code 9; on
Linux/macOS the executable finds `libpdfium` next to itself through its rpath (`$ORIGIN`, `@executable_path`).

## Deliberate differences from the old tool
None of these change the accepted command line.
- No dialogs, no key-press wait, no trial limits, no watermark; Unicode file names; long paths on Windows.
- Exit codes are non-zero on failure (2 input, 3 render, 4 output, 5 time limit, 6 format, 7 password, 9 internal);
  the old tool returned 0 for almost everything.
- An output name without an extension or with an unknown one is an error (exit 6) instead of 0-byte files; WMF and
  EMF output are not supported (exit 6). A missing output folder is created.
- `-o <existing folder>` (without `-d`) writes `<folder>/<input name>0001.tif` ... (old: 0-byte files).
- Password-protected PDFs fail with exit 7 unless `-upw`/`-opw` is given (old: nothing written, exit 0).
- TIFF `FillOrder` is 1 (most significant bit first) except for fax compressions (G3, G4, ClassF), where it stays 2.
  Readers that honour the tag see the same pixels; readers that ignore it now get correct data.
- TIFF images larger than 256 MB uncompressed are written in ~1 MB strips (old: always one strip).
- `-?`, no arguments and argument errors print pdf2img's own usage text (the same switches); `-h` also lists the
  additional switches. The additional switches (`-upw -opw -timeout -totaltimeout -workers -v -log -version`) are
  matched as whole words, so `-log` is not read as the old clustered flags `-l og`.
- `-d` reproduces the old layout `<-o folder>/<input name>/<input name>0001.<ext>`.
- JPEG output is always 3-component 4:2:0 like the old tool, also for `-b 8 -g`.
- With several failures the exit code is the most severe one, independent of timing.
- The optional log file records only errors and warnings (everything with `-v`), with the command line of the run
  before its first record and passwords replaced by `***`.
