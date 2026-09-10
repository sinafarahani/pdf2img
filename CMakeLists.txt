cmake_minimum_required(VERSION 3.21)

# pdf2img — PDF to image converter (PDFium), command-line compatible with VeryPDF PDF2Image v2.1.
# Build (see README.md, "Building from source"):
#   cmake --preset release && cmake --build --preset release              (Windows, MSVC, VCPKG_ROOT set)
#   cmake --preset linux-release && cmake --build --preset linux-release  (Linux)
#   cmake --preset macos-release && cmake --build --preset macos-release  (macOS)
# The prebuilt PDFium library (bblanchon/pdfium-binaries) is downloaded into third_party/pdfium at configure time.
project(pdf2img VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
if(MSVC)
  # Static CRT (single exe + pdfium.dll, no VC redist dependency on the target machine).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# --- PDFium (prebuilt, BSD-3) ---
set(PDF2IMG_PDFIUM_RELEASE "chromium/8009" CACHE STRING "pdfium-binaries release tag")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  set(_pdfium_cpu "${CMAKE_OSX_ARCHITECTURES}")
else()
  set(_pdfium_cpu "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(_pdfium_cpu MATCHES "^(aarch64|arm64|ARM64)$")
  set(_pdfium_arch arm64)
else()
  set(_pdfium_arch x64)
endif()
if(WIN32)
  set(_pdfium_asset "pdfium-win-${_pdfium_arch}.tgz")
elseif(APPLE)
  set(_pdfium_asset "pdfium-mac-${_pdfium_arch}.tgz")
else()
  set(_pdfium_asset "pdfium-linux-${_pdfium_arch}.tgz")
endif()
if(WIN32)
  set(PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium")   # also used by new.vcxproj
else()
  # One folder per platform, so that Windows and Linux/WSL builds from the same checkout do not replace each other's.
  string(REPLACE ".tgz" "" _pdfium_name "${_pdfium_asset}")
  set(PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/${_pdfium_name}")
endif()
set(PDF2IMG_PDFIUM_DIR "${PDFIUM_DIR}" CACHE INTERNAL "PDFium package used by this build (read by scripts/package.ps1)")
set(_pdfium_stamp "${PDFIUM_DIR}/PINNED_RELEASE.txt")
set(_pdfium_want "${PDF2IMG_PDFIUM_RELEASE} ${_pdfium_asset}")
set(_pdfium_have "")
if(EXISTS "${_pdfium_stamp}")
  file(READ "${_pdfium_stamp}" _pdfium_have)
  string(STRIP "${_pdfium_have}" _pdfium_have)
endif()
if(NOT _pdfium_have STREQUAL _pdfium_want)
  set(_url "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDF2IMG_PDFIUM_RELEASE}/${_pdfium_asset}")
  set(_tgz "${CMAKE_BINARY_DIR}/${_pdfium_asset}")
  message(STATUS "Downloading ${_url}")
  file(DOWNLOAD "${_url}" "${_tgz}" STATUS _st TLS_VERIFY ON)
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    message(FATAL_ERROR "PDFium download failed: ${_st}")
  endif()
  file(REMOVE_RECURSE "${PDFIUM_DIR}")
  file(MAKE_DIRECTORY "${PDFIUM_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${_tgz}" DESTINATION "${PDFIUM_DIR}")
  file(REMOVE "${_tgz}")
  if(APPLE)
    # The prebuilt dylib's install name is "./libpdfium.dylib" (relative to the working directory): make it
    # @rpath-relative so that the executable finds it next to itself, then re-sign it (required on Apple silicon).
    execute_process(COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${PDFIUM_DIR}/lib/libpdfium.dylib" RESULT_VARIABLE _r1)
    execute_process(COMMAND codesign --force --sign - "${PDFIUM_DIR}/lib/libpdfium.dylib" RESULT_VARIABLE _r2)
    if(NOT _r1 EQUAL 0 OR NOT _r2 EQUAL 0)
      message(FATAL_ERROR "cannot adjust libpdfium.dylib (install_name_tool: ${_r1}, codesign: ${_r2})")
    endif()
  endif()
  file(WRITE "${_pdfium_stamp}" "${_pdfium_want}\n")
endif()
# PDFium_ROOT: the package's own find_library/find_path calls search the pinned copy before any system-wide one.
set(PDFium_ROOT "${PDFIUM_DIR}")
find_package(PDFium REQUIRED PATHS "${PDFIUM_DIR}" NO_DEFAULT_PATH)
if(UNIX AND NOT APPLE)
  # Link as -lpdfium so that the executable does not record the build machine's path to the library.
  set_target_properties(pdfium PROPERTIES IMPORTED_NO_SONAME TRUE)
endif()

# --- vcpkg libraries ---
find_package(TIFF REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(GIF REQUIRED)
find_package(qpdf CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(PDF2IMG_CORE_SOURCES
  src/cli.cpp
  src/common.cpp
  src/config.cpp
  src/fsutil.cpp
  src/ipc.cpp
  src/log.cpp
  src/naming.cpp
  src/pipeline.cpp
  src/child_process.cpp
  src/supervisor.cpp
  src/worker.cpp
  src/image/convert.cpp
  src/encode/tiff_writer.cpp
  src/encode/jpeg_writer.cpp
  src/encode/png_writer.cpp
  src/encode/gif_writer.cpp
  src/encode/simple_writers.cpp
  src/render/pdf_document.cpp
  src/render/repair_qpdf.cpp
)
set(PDF2IMG_HEADERS
  src/cli.h src/common.h src/config.h src/fsutil.h src/ipc.h src/log.h src/naming.h src/pipeline.h src/child_process.h
  src/supervisor.h src/worker.h src/version.h
  src/image/image.h src/image/convert.h src/image/palettes.h
  src/encode/encoders.h src/render/pdf_document.h
)

# Everything except main(): shared by the executable and the unit tests.
add_library(pdf2img_core STATIC ${PDF2IMG_CORE_SOURCES} ${PDF2IMG_HEADERS})
target_include_directories(pdf2img_core PUBLIC src)
target_link_libraries(pdf2img_core PUBLIC pdfium TIFF::TIFF JPEG::JPEG PNG::PNG ZLIB::ZLIB GIF::GIF qpdf::libqpdf Threads::Threads)
if(WIN32)
  target_compile_definitions(pdf2img_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  target_compile_options(pdf2img_core PUBLIC /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  target_compile_options(pdf2img_core PUBLIC $<$<CONFIG:Release>:/O2 /Ot /Oi /GL /Gy /Gw>)
else()
  target_compile_options(pdf2img_core PUBLIC -Wall -Wextra)
endif()
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  # vcpkg's headers must win over copies in default search paths (e.g. Homebrew's libjpeg in /usr/local/include on
  # Intel Macs, whose JPEG-8 API does not match the vcpkg library that is linked).
  target_include_directories(pdf2img_core BEFORE PUBLIC "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
  # qpdf's imported target references bare library names (zs/zsd/jpeg); make the vcpkg lib dirs searchable.
  target_link_directories(pdf2img_core PUBLIC
    "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib>")
endif()

add_executable(pdf2img src/main.cpp)
target_link_libraries(pdf2img PRIVATE pdf2img_core)
if(WIN32)
  target_sources(pdf2img PRIVATE src/app.rc src/app.manifest)
  target_link_options(pdf2img PRIVATE /SUBSYSTEM:CONSOLE /DELAYLOAD:pdfium.dll)
  target_link_libraries(pdf2img PRIVATE delayimp)
endif()

# --- unit tests (no external framework) ---
add_executable(pdf2img_tests tests/unit/test_main.cpp)
target_link_libraries(pdf2img_tests PRIVATE pdf2img_core)
enable_testing()
add_test(NAME unit COMMAND pdf2img_tests)

foreach(_t pdf2img pdf2img_tests)
  if(MSVC)
    target_link_options(${_t} PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
  endif()
  if(APPLE)
    set_target_properties(${_t} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path")
  elseif(UNIX)
    # Find libpdfium.so next to the executable; libstdc++/libgcc linked in so the binary runs on other distributions.
    set_target_properties(${_t} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
    target_link_options(${_t} PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  # The PDFium library must sit next to the executable.
  add_custom_command(TARGET ${_t} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFium_LIBRARY}" "$<TARGET_FILE_DIR:${_t}>"
    COMMENT "Copying the PDFium library next to ${_t}")
endforeach()
