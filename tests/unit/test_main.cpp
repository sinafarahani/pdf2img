// Unit tests (no framework): naming rules, CLI parsing, rounding, IPC escaping, conversions, encoders, platform helpers.
#include "cli.h"
#include "common.h"
#include "config.h"
#include "fsutil.h"
#include "ipc.h"
#include "naming.h"
#include "pipeline.h"
#include "image/convert.h"
#include "image/palettes.h"
#include "encode/encoders.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace p2i;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++g_failures; printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static std::vector<wchar_t*> argv_of(std::vector<std::wstring>& args) {
    std::vector<wchar_t*> v;
    for (auto& a : args) v.push_back(a.data());
    return v;
}

static void touch(const std::wstring& p) {
    if (FILE* f = open_file_for_writing(p)) fclose(f);
}

static void test_rounding() {
    // Observed sizes of the old tool.
    CHECK_EQ(points_to_pixels(612, 101), 858);
    CHECK_EQ(points_to_pixels(792, 101), 1111);
    CHECK_EQ(points_to_pixels(400, 101), 561);
    CHECK_EQ(points_to_pixels(300, 101), 421);
    CHECK_EQ(points_to_pixels(1133.86, 101), 1591);
    CHECK_EQ(points_to_pixels(1644.09, 101), 2306);
    CHECK_EQ(points_to_pixels(4320, 101), 6060);
    CHECK_EQ(points_to_pixels(2880, 101), 4040);
    CHECK_EQ(points_to_pixels(612, 300), 2550);
    CHECK_EQ(points_to_pixels(612, 200), 1700);
    CHECK_EQ(points_to_pixels(792, 100), 1100);
}

static void test_cli() {
    {
        std::vector<std::wstring> a = {L"pdf2img", L"-i", L"C:\\in.pdf", L"-o", L"C:\\out.tif", L"-c", L"lzw", L"-r", L"300"};
        auto v = argv_of(a); Options o; std::wstring m;
        CHECK(parse_command_line(static_cast<int>(v.size()), v.data(), o, m) == ParseStatus::Ok);
        CHECK_EQ(o.input, std::wstring(L"C:\\in.pdf"));
        CHECK_EQ(o.output, std::wstring(L"C:\\out.tif"));
        CHECK(o.compression == Compression::Lzw);
        CHECK_EQ(o.dpiX, 300); CHECK_EQ(o.dpiY, 300);
    }
    {   // options after -i/-o, clustered flags, attached values, hidden -$ and -d
        std::vector<std::wstring> a = {L"pdf2img", L"-i", L"in.pdf", L"-o", L"out.tif", L"-b", L"1", L"-c", L"ClassF", L"-r", L"204x98", L"-mg", L"-$", L"-d", L"-f3", L"-l", L"7", L"-q", L"80"};
        auto v = argv_of(a); Options o; std::wstring m;
        CHECK(parse_command_line(static_cast<int>(v.size()), v.data(), o, m) == ParseStatus::Ok);
        CHECK(o.multipage && o.grayscale && o.outputIsDir);
        CHECK_EQ(o.bitCount, 1); CHECK(o.compression == Compression::ClassF);
        CHECK_EQ(o.dpiX, 204); CHECK_EQ(o.dpiY, 98);
        CHECK_EQ(o.firstPage, 3); CHECK_EQ(o.lastPage, 7); CHECK_EQ(o.quality, 80);
    }
    {   // positional input is rejected (old behaviour)
        std::vector<std::wstring> a = {L"pdf2img", L"-f", L"1", L"-l", L"9", L"C:\\input.pdf", L"-o", L"C:\\output.jpg"};
        auto v = argv_of(a); Options o; std::wstring m;
        CHECK(parse_command_line(static_cast<int>(v.size()), v.data(), o, m) == ParseStatus::Error);
    }
    {   // no args -> error; -? -> help; unknown -> error; missing -i -> error
        std::vector<std::wstring> a = {L"pdf2img"}; auto v = argv_of(a); Options o; std::wstring m;
        CHECK(parse_command_line(1, v.data(), o, m) == ParseStatus::Error);
        std::vector<std::wstring> b = {L"pdf2img", L"-?"}; auto vb = argv_of(b); Options ob;
        CHECK(parse_command_line(2, vb.data(), ob, m) == ParseStatus::Help);
        std::vector<std::wstring> c = {L"pdf2img", L"-z", L"-i", L"x.pdf"}; auto vc = argv_of(c); Options oc;
        CHECK(parse_command_line(4, vc.data(), oc, m) == ParseStatus::Error);
        std::vector<std::wstring> d = {L"pdf2img", L"-o", L"x.tif"}; auto vd = argv_of(d); Options od;
        CHECK(parse_command_line(3, vd.data(), od, m) == ParseStatus::Error);
    }
    {   // extras
        std::vector<std::wstring> a = {L"pdf2img", L"-upw", L"secret", L"-timeout", L"30", L"-workers", L"2", L"-v", L"-i", L"x.pdf"};
        auto v = argv_of(a); Options o; std::wstring m;
        CHECK(parse_command_line(static_cast<int>(v.size()), v.data(), o, m) == ParseStatus::Ok);
        CHECK_EQ(o.userPassword, std::string("secret")); CHECK_EQ(o.pageTimeoutSec, 30); CHECK_EQ(o.workers, 2); CHECK(o.verbose);
    }
    int x = 0, y = 0;
    CHECK(parse_resolution(L"300", x, y) && x == 300 && y == 300);
    CHECK(parse_resolution(L"200X300", x, y) && x == 200 && y == 300);
    CHECK(!parse_resolution(L"abc", x, y));
    CHECK(!parse_resolution(L"0", x, y));
    Compression c;
    CHECK(parse_compression(L"CLASSF196", c) && c == Compression::ClassF196);
    CHECK(parse_compression(L"PackBits", c) && c == Compression::PackBits);
    CHECK(!parse_compression(L"zip", c));
    CHECK(format_from_extension(L".TIFF") == OutFormat::Tiff);
    CHECK(format_from_extension(L".Jpeg") == OutFormat::Jpeg);
    CHECK(format_from_extension(L"") == OutFormat::Unknown);
    CHECK(format_from_extension(L".xyz") == OutFormat::Unknown);
}

static void test_naming() {
    int first, last;
    resolve_page_range(0, 0, 3, first, last); CHECK(first == 1 && last == 3);
    resolve_page_range(2, 3, 3, first, last); CHECK(first == 2 && last == 3);
    resolve_page_range(5, 0, 3, first, last); CHECK(first == 1 && last == 3);   // -f beyond -> whole document
    resolve_page_range(0, 99, 3, first, last); CHECK(first == 1 && last == 3);  // -l beyond -> clamp
    resolve_page_range(3, 2, 3, first, last); CHECK(first == 3 && last == 3);   // -l < -f -> only -f
    resolve_page_range(1, 1, 17, first, last); CHECK(first == 1 && last == 1);

    const std::wstring outDir = kPathSep == L'\\' ? L"C:\\out" : L"/out";
    const std::wstring sep(1, kPathSep);
    OutputSpec o; o.dir = outDir; o.stem = L"out"; o.ext = L".tif"; o.format = OutFormat::Tiff;
    CHECK_EQ(page_output_path(o, 1, L"%04d"), outDir + sep + L"out0001.tif");
    CHECK_EQ(page_output_path(o, 12, L"_%03d"), outDir + sep + L"out_012.tif");
    CHECK_EQ(plain_output_path(o), outDir + sep + L"out.tif");
    CHECK(uses_page_suffix(OutFormat::Tiff, false, 1));    // TIFF single-page mode: always
    CHECK(!uses_page_suffix(OutFormat::Tiff, true, 3));    // -m: never
    CHECK(!uses_page_suffix(OutFormat::Jpeg, false, 1));   // one page produced: no suffix
    CHECK(uses_page_suffix(OutFormat::Jpeg, false, 3));
    CHECK(!multipage_applies(OutFormat::Jpeg, true));
    CHECK(multipage_applies(OutFormat::Tiff, true));

    CHECK_EQ(sanitize_suffix_format(L"%04d"), std::wstring(L"%04d"));
    CHECK_EQ(sanitize_suffix_format(L"_%03i"), std::wstring(L"_%03i"));
    CHECK_EQ(sanitize_suffix_format(L"%s"), std::wstring(L"%04d"));
    CHECK_EQ(sanitize_suffix_format(L"%d%d"), std::wstring(L"%04d"));
    CHECK_EQ(sanitize_suffix_format(L"page"), std::wstring(L"%04d"));
    CHECK_EQ(format_page_suffix(L"%04d", 7), std::wstring(L"0007"));

    // resolve_jobs with a temp directory
    const std::wstring dir = path_join(temp_directory(), L"pdf2img_unit_" + std::to_wstring(current_pid()));
    const std::wstring sub = path_join(dir, L"sub");
    std::string e; CHECK(ensure_directory(sub, e));
    touch(path_join(dir, L"a.pdf")); touch(path_join(dir, L"b.pdf")); touch(path_join(dir, L"c.txt"));
    {
        Options op; op.input = path_join(dir, L"a.pdf"); std::vector<JobProblem> pr;
        auto jobs = resolve_jobs(op, pr);
        CHECK(pr.empty()); CHECK_EQ(jobs.size(), size_t(1));
        if (!jobs.empty()) { CHECK_EQ(jobs[0].out.dir, dir); CHECK_EQ(jobs[0].out.stem, std::wstring(L"a")); CHECK_EQ(jobs[0].out.ext, std::wstring(L".tif")); }
    }
    {
        Options op; op.input = path_join(dir, L"*.pdf"); op.output = path_join(sub, L"*.jpg"); std::vector<JobProblem> pr;
        auto jobs = resolve_jobs(op, pr);
        CHECK(pr.empty()); CHECK_EQ(jobs.size(), size_t(2));
        if (jobs.size() == 2) { CHECK_EQ(jobs[0].out.stem, std::wstring(L"a")); CHECK_EQ(jobs[1].out.stem, std::wstring(L"b")); CHECK_EQ(jobs[1].out.dir, sub); CHECK(jobs[1].out.format == OutFormat::Jpeg); }
    }
    {
        Options op; op.input = path_join(dir, L"a.pdf"); op.output = sub; op.outputIsDir = true; std::vector<JobProblem> pr;
        auto jobs = resolve_jobs(op, pr);
        CHECK_EQ(jobs.size(), size_t(1));
        if (!jobs.empty()) { CHECK_EQ(jobs[0].out.dir, path_join(sub, L"a")); CHECK_EQ(jobs[0].out.stem, std::wstring(L"a")); CHECK_EQ(jobs[0].out.ext, std::wstring(L".tif")); } // -d: per-input subdirectory (old behaviour)
    }
    {
        Options op; op.input = path_join(dir, L"a.pdf"); op.output = path_join(dir, L"out.xyz"); std::vector<JobProblem> pr;
        auto jobs = resolve_jobs(op, pr);
        CHECK_EQ(pr.size(), size_t(1)); if (!pr.empty()) CHECK_EQ(pr[0].exitCode, (int)EXIT_UNSUPPORTED_FORMAT);
    }
    {
        Options op; op.input = path_join(dir, L"missing.pdf"); std::vector<JobProblem> pr;
        auto jobs = resolve_jobs(op, pr);
        CHECK(jobs.empty()); CHECK_EQ(pr.size(), size_t(1)); if (!pr.empty()) CHECK_EQ(pr[0].exitCode, (int)EXIT_INPUT);
    }
    delete_file_quiet(path_join(dir, L"a.pdf")); delete_file_quiet(path_join(dir, L"b.pdf")); delete_file_quiet(path_join(dir, L"c.txt"));
    remove_directory_quiet(sub); remove_directory_quiet(dir);
    CHECK(!directory_exists(dir));
}

static void test_platform() {
    // UTF-8 round trip (Persian text) and raw bytes that are not UTF-8.
    const std::string persian = "\xD9\xBE\xD9\x88\xD8\xB4\xD9\x87 \xD8\xAA\xD8\xB3\xD8\xAA";
    CHECK_EQ(narrow(widen(persian)), persian);
    CHECK_EQ(widen(persian).size(), size_t(8));
#ifndef _WIN32
    const std::string raw = std::string("a\xFF\xC3(b\xE2\x82", 7);
    CHECK_EQ(narrow(widen(raw)), raw);
    CHECK_EQ(make_absolute(L"/a/./b/../c/"), std::wstring(L"/a/c/"));
    CHECK_EQ(make_absolute(L"/../x"), std::wstring(L"/x"));
    CHECK_EQ(path_dir(L"/x.pdf"), std::wstring(L"/"));
    CHECK_EQ(path_name(L"/dir/a\\b.pdf"), std::wstring(L"a\\b.pdf")); // a backslash is part of a file name here
#else
    CHECK_EQ(path_dir(L"C:\\x.pdf"), std::wstring(L"C:\\"));
    CHECK_EQ(path_name(L"C:\\dir/a.pdf"), std::wstring(L"a.pdf"));
#endif
    CHECK(!make_absolute(L"rel.pdf").empty());
    CHECK_EQ(path_name(make_absolute(L"rel.pdf")), std::wstring(L"rel.pdf"));
    CHECK(!exe_path().empty());
    CHECK(tick_ms() > 0);
    // exclusive create
    const std::wstring marker = path_join(temp_directory(), L"pdf2img_unit_marker_" + std::to_wstring(current_pid()));
    delete_file_quiet(marker);
    CHECK(create_new_file(marker));
    CHECK(!create_new_file(marker));
    CHECK(file_exists(marker) && !directory_exists(marker));
    delete_file_quiet(marker);
    CHECK(!path_exists(marker));
    // temp names for very long outputs stay short
    const std::wstring longTarget = path_join(temp_directory(), std::wstring(240, L'x') + L".tif");
    CHECK(path_name(temp_name_for(longTarget, 42)).rfind(L"~p2i_42_", 0) == 0);
    CHECK_EQ(temp_name_for(path_join(temp_directory(), L"a.tif"), 42), path_join(temp_directory(), L"a.tif.42.tmp"));
}

static void test_redaction() {
    std::vector<std::wstring> a = {L"C:\\app\\0.exe", L"-upw", L"S3cret", L"-i", L"C:\\in dir\\a.pdf", L"--opw", L"Own3r", L"-o", L"x.tif"};
    auto v = argv_of(a);
    std::string s = loggable_command_line(static_cast<int>(v.size()), v.data());
    CHECK(s.find("S3cret") == std::string::npos);
    CHECK(s.find("Own3r") == std::string::npos);
    CHECK(s.find("-upw *** ") != std::string::npos);
    CHECK(s.find("--opw *** ") != std::string::npos);
    CHECK(s.find("\"C:\\in dir\\a.pdf\"") != std::string::npos);
    std::vector<std::wstring> b = {L"0.exe", L"-i", L"a.pdf", L"-upw"}; // dangling switch: nothing to redact, no crash
    auto vb = argv_of(b);
    CHECK_EQ(loggable_command_line(static_cast<int>(vb.size()), vb.data()), std::string("0.exe -i a.pdf -upw"));
    std::vector<std::wstring> c = {L"0.exe", L"-log", L"x.log", L"-UPW", L"Secret9", L"-i", L"a.pdf"}; // any case; parse failed
    auto vc = argv_of(c);
    std::string sc = loggable_command_line(static_cast<int>(vc.size()), vc.data(), false);
    CHECK(sc.find("Secret9") == std::string::npos);
    CHECK(sc.find("a.pdf") == std::string::npos);     // everything after the switch masked when parsing failed
    CHECK(sc.find("-log x.log") != std::string::npos); // arguments before it are kept
    std::vector<std::wstring> d = {L"0.exe", L"-upw=Secret9", L"-i", L"a.pdf"}; // attached value
    auto vd = argv_of(d);
    std::string sd = loggable_command_line(static_cast<int>(vd.size()), vd.data());
    CHECK(sd.find("Secret9") == std::string::npos);
    CHECK(sd.find("-upw***") != std::string::npos);
    CHECK(has_password_switch(static_cast<int>(vc.size()), vc.data()));
    std::vector<std::wstring> e = {L"0.exe", L"-i", L"a.pdf", L"-o", L"b.tif"};
    auto ve = argv_of(e);
    CHECK(!has_password_switch(static_cast<int>(ve.size()), ve.data()));
    {   // an option value that is literally "-upw" does not shift the masking: the parser's positions are used
        std::vector<std::wstring> a2 = {L"0.exe", L"-i", L"-upw", L"-upw", L"S3cret"};
        auto v2 = argv_of(a2); Options o2; std::wstring m2;
        CHECK(parse_command_line(static_cast<int>(v2.size()), v2.data(), o2, m2) == ParseStatus::Ok);
        std::string s2 = loggable_command_line(static_cast<int>(v2.size()), v2.data(), true, &o2.passwordArgIndices);
        CHECK(s2.find("S3cret") == std::string::npos);
        CHECK(s2.find("-i -upw -upw ***") != std::string::npos);
    }
    {   // a broken quote folds the password into another argument: rejected, and masked in the log
        std::vector<std::wstring> a3 = {L"0.exe", L"-i", L"a.pdf", L"-o", L"C:\\out\" -upw S3cret"};
        auto v3 = argv_of(a3); Options o3; std::wstring m3;
        CHECK(parse_command_line(static_cast<int>(v3.size()), v3.data(), o3, m3) == ParseStatus::Error);
        CHECK(has_password_switch(static_cast<int>(v3.size()), v3.data()));
        std::string s3 = loggable_command_line(static_cast<int>(v3.size()), v3.data(), false);
        CHECK(s3.find("S3cret") == std::string::npos);
        CHECK(s3.find("-i a.pdf -o") != std::string::npos);
    }
}

static void test_ipc() {
    std::string line = ipc::join({"PAGE", "3", "C:\\dir with space\\a%b\tc.pdf", "12", "out\n.tif"});
    auto f = ipc::split(line);
    CHECK_EQ(f.size(), size_t(5));
    CHECK_EQ(f[2], std::string("C:\\dir with space\\a%b\tc.pdf"));
    CHECK_EQ(f[4], std::string("out\n.tif"));
    CHECK_EQ(ipc::split("READY\r\n").size(), size_t(1));
}

static void test_convert() {
    Image bgr; bgr.allocate(4, 1, 24);
    uint8_t* p = bgr.row(0);
    p[0] = 255; p[1] = 0; p[2] = 0;      // blue
    p[3] = 0; p[4] = 0; p[5] = 255;      // red
    p[6] = 255; p[7] = 255; p[8] = 255;  // white
    p[9] = 0; p[10] = 0; p[11] = 0;      // black
    Image g = to_gray(bgr);
    CHECK_EQ(static_cast<int>(g.row(0)[0]), 29);   // blue luma
    CHECK_EQ(static_cast<int>(g.row(0)[1]), 76);   // red luma
    CHECK_EQ(static_cast<int>(g.row(0)[2]), 255);
    Image b = gray_to_bilevel(g, 128);
    CHECK_EQ(static_cast<int>(b.row(0)[0]), 0xD0); // blue, red -> black(1); white -> 0; black -> 1 : 1101 0000
    Image p16 = to_palette(bgr, kPaletteVga16, 16, 4);
    CHECK_EQ(static_cast<int>(p16.row(0)[0]), (12 << 4) | 9);  // blue = 0x0000FF index 12, red index 9
    CHECK_EQ(static_cast<int>(p16.row(0)[1]), (15 << 4) | 0);  // white 15, black 0
    Image p256 = to_palette(bgr, kPaletteHalftone256, 256, 8);
    CHECK_EQ(kPaletteHalftone256[p256.row(0)[2]], 0xFFFFFFu);
    CHECK_EQ(kPaletteHalftone256[p256.row(0)[3]], 0x000000u);
    // resample: 4x1 gray -> 2x1 averages pairs
    Image g2; g2.allocate(4, 1, 8); g2.gray = true; g2.row(0)[0] = 0; g2.row(0)[1] = 100; g2.row(0)[2] = 200; g2.row(0)[3] = 200;
    Image r = resample_gray(g2, 2, 1);
    CHECK_EQ(static_cast<int>(r.row(0)[0]), 50); CHECK_EQ(static_cast<int>(r.row(0)[1]), 200);

    // spec derivation
    ConversionSpec s = make_spec(OutFormat::Tiff, 0, 0, 24, false, Compression::ClassF, 0, 101, 90, true, true, 1u << 30);
    CHECK(s.bitCount == 1 && s.classF && s.dpiX == 204 && s.dpiY == 98);
    s = make_spec(OutFormat::Tiff, 0, 0, 24, false, Compression::G4, 0, 101, 90, true, true, 1u << 30);
    CHECK(s.bitCount == 1 && !s.classF && s.dpiX == 101);
    s = make_spec(OutFormat::Jpeg, 300, 300, 8, true, Compression::Lzw, 0, 101, 90, true, true, 1u << 30);
    CHECK(s.bitCount == 8 && s.gray && s.compression == Compression::Default && s.jpegQuality == 90);
    s = make_spec(OutFormat::Jpeg, 0, 0, 8, false, Compression::Default, 20, 101, 90, true, true, 1u << 30);
    CHECK(s.bitCount == 24 && s.jpegQuality == 20);
    s = make_spec(OutFormat::Gif, 0, 0, 0, false, Compression::Default, 0, 101, 90, true, true, 1u << 30);
    CHECK(s.bitCount == 8 && !s.gray);
    s = make_spec(OutFormat::Tiff, 0, 0, 0, true, Compression::Default, 0, 101, 90, true, true, 1u << 30);
    CHECK(s.bitCount == 24 && !s.gray); // -g without -b 8 has no effect
}

static void test_encoders() {
    const std::wstring base = path_join(temp_directory(), L"pdf2img_enc_" + std::to_wstring(current_pid()));
    Image img; img.allocate(37, 11, 24); img.dpiX = 101; img.dpiY = 101;
    for (int y = 0; y < img.height; ++y) for (int x = 0; x < img.width; ++x) { uint8_t* p = img.row(y) + x * 3; p[0] = static_cast<uint8_t>(x * 6); p[1] = static_cast<uint8_t>(y * 20); p[2] = 200; }
    EncodeParams ep;
    std::string err;
    struct Case { OutFormat f; const wchar_t* ext; } cases[] = {{OutFormat::Tiff, L".tif"}, {OutFormat::Jpeg, L".jpg"}, {OutFormat::Png, L".png"}, {OutFormat::Bmp, L".bmp"}, {OutFormat::Pcx, L".pcx"}, {OutFormat::Tga, L".tga"}};
    for (auto& c : cases) {
        std::wstring p = base + c.ext;
        bool ok = write_image(c.f, p, img, ep, err);
        CHECK(ok); if (!ok) printf("  %ls: %s\n", c.ext, err.c_str());
        CHECK(file_size_of(p) > 100);
        delete_file_quiet(p);
    }
    // palette/gray/1-bit variants
    Image g = to_gray(img);
    Image b = gray_to_bilevel(g, 128);
    Image p4 = to_palette(img, kPaletteVga16, 16, 4);
    Image p8 = to_palette(img, kPaletteHalftone256, 256, 8);
    struct V { const Image* im; OutFormat f; const wchar_t* name; } variants[] = {
        {&g, OutFormat::Jpeg, L"_g.jpg"}, {&g, OutFormat::Tiff, L"_g.tif"}, {&g, OutFormat::Png, L"_g.png"}, {&g, OutFormat::Bmp, L"_g.bmp"}, {&g, OutFormat::Gif, L"_g.gif"}, {&g, OutFormat::Pcx, L"_g.pcx"}, {&g, OutFormat::Tga, L"_g.tga"},
        {&b, OutFormat::Tiff, L"_b.tif"}, {&b, OutFormat::Png, L"_b.png"}, {&b, OutFormat::Bmp, L"_b.bmp"}, {&b, OutFormat::Gif, L"_b.gif"}, {&b, OutFormat::Pcx, L"_b.pcx"},
        {&p4, OutFormat::Tiff, L"_p4.tif"}, {&p4, OutFormat::Png, L"_p4.png"}, {&p4, OutFormat::Bmp, L"_p4.bmp"}, {&p4, OutFormat::Gif, L"_p4.gif"},
        {&p8, OutFormat::Tiff, L"_p8.tif"}, {&p8, OutFormat::Png, L"_p8.png"}, {&p8, OutFormat::Bmp, L"_p8.bmp"}, {&p8, OutFormat::Gif, L"_p8.gif"}, {&p8, OutFormat::Pcx, L"_p8.pcx"}, {&p8, OutFormat::Tga, L"_p8.tga"},
    };
    for (auto& v : variants) {
        std::wstring p = base + v.name;
        bool ok = write_image(v.f, p, *v.im, ep, err);
        CHECK(ok); if (!ok) printf("  %ls: %s\n", v.name, err.c_str());
        CHECK(file_size_of(p) > 40);
        delete_file_quiet(p);
    }
    // TIFF compressions incl. multi-page and fax
    struct C { Compression c; const Image* im; } comps[] = {{Compression::None, &img}, {Compression::Lzw, &img}, {Compression::Jpeg, &img}, {Compression::PackBits, &img}, {Compression::G3, &b}, {Compression::G4, &b}, {Compression::Lzw, &p8}, {Compression::Lzw, &b}};
    for (auto& c : comps) {
        std::wstring p = base + L"_c.tif";
        EncodeParams e2; e2.compression = c.c; e2.faxFillOrder = (c.c == Compression::G3 || c.c == Compression::G4);
        TiffWriter w;
        bool ok = w.open(p, err) && w.add_page(*c.im, e2, 0, 2, err) && w.add_page(*c.im, e2, 1, 2, err) && w.close(err);
        CHECK(ok); if (!ok) printf("  compression %s: %s\n", compression_name(c.c), err.c_str());
        delete_file_quiet(p);
    }
}

int main() {
    test_rounding();
    test_cli();
    test_naming();
    test_platform();
    test_ipc();
    test_redaction();
    test_convert();
    test_encoders();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
