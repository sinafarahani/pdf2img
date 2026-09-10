# Test PDFs

Small synthetic documents used by the unit, compatibility (`run_golden.ps1`) and robustness (`run_robustness.ps1`)
tests. They contain only generated test content.

| File | What it is |
|---|---|
| `one.pdf` | 1 page, US Letter (612 x 792 pt) |
| `three.pdf` | 3 pages, US Letter |
| `crop.pdf` | a page with a 400 x 300 pt crop box (the output must follow the crop box) |
| `huge.pdf` | a 4320 x 2880 pt (60 x 40 in) page |
| `enc.pdf` | encrypted, user password `secret` |
| `ownerpw.pdf` | encrypted with an owner password only (opens without a password) |
| `corrupt.pdf` | 84 bytes that are not a PDF |
| `damaged.pdf` | `three.pdf` with a broken header (`%XDF-` instead of `%PDF-`): PDFium refuses it, qpdf repairs it |

`../golden.json` holds the cases that were run against the old VeryPDF tool: for each command line, its exit code
and the name, pixel size, bit depth, resolution and TIFF tags of every file it produced.
