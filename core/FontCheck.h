// oc::FontCheck — 字体可用性检测（ADR A8）
// 核心（纯逻辑，可测）：收集文档字体 → 对照已安装集合 → 缺失清单 + 回退建议。
// 系统字体枚举由宿主注入（Qt: QFontDatabase::families()；测试: 假集合）。
#pragma once
#include "Document.h"
#include <set>
#include <string>
#include <vector>

namespace oc {

struct FontIssue {
    std::string missing;     // 文档要求但未安装
    std::string fallback;    // 建议显示回退（可能为空 = 无建议）
};

class FontCheck {
public:
    // 文档实际使用的字体（eastAsia 优先 + ascii；去重、保持出现顺序）
    static std::vector<std::string> collectUsedFonts(const Document& doc);

    // 对照检查。installed：宿主枚举的系统字体名（大小写不敏感比较）。
    static std::vector<FontIssue> check(const std::vector<std::string>& used,
                                        const std::set<std::string>& installed);

    // 标书常见回退表（数据驱动，后续可扩展为外部配置）
    static const std::vector<std::pair<std::string, std::string>>& fallbackTable();
};

} // namespace oc
