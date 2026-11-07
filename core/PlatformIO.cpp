#include "PlatformIO.h"
#include <cstdint>

namespace oc {
#ifdef _WIN32
namespace {

// UTF-8 → UTF-16 解码（零依赖；非法序列以 U+FFFD 兜底，不失败——路径容错）
std::wstring utf8ToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD;
        int extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { ++i; w.push_back(0xFFFD); continue; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + (size_t)k >= s.size() ||
                ((unsigned char)s[i + k] >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        }
        if (!ok) { ++i; w.push_back(0xFFFD); continue; }
        i += (size_t)extra + 1;
        if (cp > 0x10FFFF) cp = 0xFFFD;
        if (cp <= 0xFFFF) w.push_back((wchar_t)cp);
        else {                                    // 代理对
            cp -= 0x10000;
            w.push_back((wchar_t)(0xD800 | (cp >> 10)));
            w.push_back((wchar_t)(0xDC00 | (cp & 0x3FF)));
        }
    }
    return w;
}

} // namespace

FILE* fopenUtf8(const std::string& utf8Path, const char* mode) {
    wchar_t wmode[4] = {0};
    for (int i = 0; mode[i] && i < 3; ++i) wmode[i] = (wchar_t)mode[i];
    return _wfopen(utf8ToWide(utf8Path).c_str(), wmode);
}

#else // 非 Windows：UTF-8 原生路径

FILE* fopenUtf8(const std::string& utf8Path, const char* mode) {
    return fopen(utf8Path.c_str(), mode);
}

#endif
} // namespace oc
