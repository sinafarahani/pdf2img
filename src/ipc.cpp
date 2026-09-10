#include "ipc.h"

namespace p2i {
namespace ipc {

static const char kHex[] = "0123456789ABCDEF";

std::string escape(std::string_view f) {
    std::string out;
    out.reserve(f.size());
    for (unsigned char c : f) {
        if (c == '\t' || c == '\n' || c == '\r' || c == '%') {
            out += '%'; out += kHex[c >> 4]; out += kHex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string unescape(std::string_view f) {
    std::string out;
    out.reserve(f.size());
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '%' && i + 2 < f.size()) {
            int a = hexval(f[i + 1]), b = hexval(f[i + 2]);
            if (a >= 0 && b >= 0) { out += static_cast<char>((a << 4) | b); i += 2; continue; }
        }
        out += f[i];
    }
    return out;
}

std::string join(const std::vector<std::string>& fields) {
    std::string line;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) line += '\t';
        line += escape(fields[i]);
    }
    line += '\n';
    return line;
}

std::vector<std::string> split(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t t = line.find('\t', start);
        if (t == std::string_view::npos) { out.push_back(unescape(line.substr(start))); break; }
        out.push_back(unescape(line.substr(start, t - start)));
        start = t + 1;
    }
    return out;
}

} // namespace ipc
} // namespace p2i
