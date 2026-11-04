#include "Clipboard.h"

namespace oc {

std::string ClipboardFragment::plainText() const {
    std::string s;
    for (size_t i = 0; i < paras.size(); ++i) {
        if (i) s += "\n";
        for (const auto& r : paras[i].runs) s += r.text;
    }
    return s;
}

ClipboardFragment ClipboardFragment::fromPlainText(const std::string& utf8) {
    ClipboardFragment f;
    size_t start = 0;
    auto pushPara = [&](const std::string& text) {
        if (text.empty()) return;
        Paragraph p;
        TextRun r;
        r.text = text;
        p.runs = {r};
        f.paras.push_back(std::move(p));
    };
    for (;;) {
        size_t nl = utf8.find('\n', start);
        if (nl == std::string::npos) { pushPara(utf8.substr(start)); break; }
        pushPara(utf8.substr(start, nl - start));
        start = nl + 1;
    }
    if (f.paras.empty()) f.paras.emplace_back();      // 保底空段
    return f;
}

} // namespace oc
