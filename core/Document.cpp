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
