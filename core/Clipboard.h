// oc::Clipboard — 富文本剪贴板数据模型（M2.6）
// 设计：内部富文本片段（保留段落/run 格式）；与系统剪贴板的互通由 UI 层负责
// （Qt: QClipboard；纯文本走 QString，富文本走 HTML/自定义 MIME——M3）。
#pragma once
#include "Document.h"
#include <string>
#include <vector>

namespace oc {

struct ClipboardFragment {
    std::vector<Paragraph> paras;      // 跨段复制时保留段落结构

    bool empty() const { return paras.empty(); }
    // 纯文本视图（段落间 '\n'）——同步给系统剪贴板用
    std::string plainText() const;
    // 从纯文本构造（外部粘贴入口）
    static ClipboardFragment fromPlainText(const std::string& utf8);
};

} // namespace oc
