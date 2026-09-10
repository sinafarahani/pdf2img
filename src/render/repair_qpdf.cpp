// qpdf repair pass: rewrite a damaged/encrypted PDF into a clean temporary file.
#include "pdf_document.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4458) // third-party header: declaration hides class member
#endif
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFWriter.hh>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <exception>

namespace p2i {

bool qpdf_repair(const std::wstring& in, const std::string& password, const std::wstring& outTemp, std::string& err, bool* passwordError) {
    if (passwordError) *passwordError = false;
    try {
        QPDF q;
        q.setAttemptRecovery(true);
        q.setSuppressWarnings(true);
        q.setMaxWarnings(1000);
        // qpdf interprets file names as UTF-8 on Windows (and passes the bytes through elsewhere).
        q.processFile(narrow(in).c_str(), password.empty() ? nullptr : password.c_str());
        QPDFWriter w(q, narrow(outTemp).c_str());
        w.setPreserveEncryption(false);      // write a decrypted copy (if we could open it at all)
        w.setObjectStreamMode(qpdf_o_preserve);
        w.setStreamDataMode(qpdf_s_preserve); // do not recompress page content
        w.setDecodeLevel(qpdf_dl_none);
        w.write();
        return true;
    } catch (const QPDFExc& e) {
        err = e.what();
        if (passwordError && e.getErrorCode() == qpdf_e_password) *passwordError = true;
    } catch (const std::exception& e) {
        err = e.what();
    } catch (...) {
        err = "unknown qpdf error";
    }
    return false;
}

} // namespace p2i
