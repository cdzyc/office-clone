#include "Document.h"

namespace oc {

std::string Document::summary() const {
    std::string s;
    s += "paragraphs=" + std::to_string(stats.paragraphs);
    s += " tables=" + std::to_string(stats.tables);
    s += " images=" + std::to_string(stats.images);
    s += " mediaEntries=" + std::to_string(mediaIndex.size());
    s += " mediaBytes=" + std::to_string(stats.mediaBytes);
    s += " textChars=" + std::to_string(stats.textChars);
    s += " page=" + std::to_string(page.pageWidthTw) + "x" + std::to_string(page.pageHeightTw) + "tw";
    return s;
}

} // namespace oc

namespace oc {

static uint32_t utf8Chars(const std::string& s) {
    uint32_t n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        ++n;
    }
    return n;
}

void Document::recountStats() {
    auto& st = stats;
    st.paragraphs = st.tables = st.images = 0;
    st.textChars = st.unicodeChars = 0;
    for (const auto& n : body) {
        if (n.kind == Node::Kind::Table) {
            ++st.tables;
            for (const auto& row : n.table.rows)
                for (const auto& cell : row.cells)
                    for (const auto& p : cell.paragraphs) {
                        ++st.paragraphs;
                        st.images += (uint32_t)p.images.size();
                        for (const auto& r : p.runs) {
                            st.textChars += (uint32_t)r.text.size();
                            st.unicodeChars += utf8Chars(r.text);
                        }
                    }
        } else {
            ++st.paragraphs;
            st.images += (uint32_t)n.paragraph.images.size();
            for (const auto& r : n.paragraph.runs) {
                st.textChars += (uint32_t)r.text.size();
                st.unicodeChars += utf8Chars(r.text);
            }
        }
    }
}


} // namespace oc
