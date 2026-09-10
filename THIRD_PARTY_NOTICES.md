# Third-party software

pdf2img's own source code is licensed under the MIT license (see [LICENSE](LICENSE)). The executables are built
with the components below. Every release archive contains their complete license texts in its `licenses/` folder
(taken from the pdfium-binaries package and from the copyright files that vcpkg installs).

| Component | Used for | License |
|---|---|---|
| [PDFium](https://pdfium.googlesource.com/pdfium/), prebuilt by [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) | rendering PDF pages; shipped as `pdfium.dll` / `libpdfium.so` / `libpdfium.dylib` | BSD-3-Clause. The library contains FreeType, ICU, libjpeg-turbo, libpng, zlib, OpenJPEG, Little CMS, AGG, Abseil, fast_float, simdutf and LLVM libc code under their own permissive licenses. |
| [qpdf](https://github.com/qpdf/qpdf) | repairing damaged PDF files | Apache-2.0 |
| [libtiff](https://libtiff.gitlab.io/libtiff/) | TIFF output | libtiff license (BSD-style) |
| [libjpeg-turbo](https://libjpeg-turbo.org/) | JPEG output (and JPEG-compressed TIFF) | IJG license, BSD-3-Clause, zlib license |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | PNG output | PNG Reference Library License v2 |
| [zlib](https://zlib.net/) | compression | zlib license |
| [giflib](https://giflib.sourceforge.net/) | GIF output | MIT |

This software is based in part on the work of the Independent JPEG Group.

The output formats and command line reproduce the behaviour of VeryPDF "PDF To Image Converter" 2.1. pdf2img
contains no VeryPDF code and is not affiliated with or endorsed by VeryPDF.
