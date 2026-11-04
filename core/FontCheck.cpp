#include "FontCheck.h"
#include <algorithm>
#include <cctype>

namespace oc {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

} // namespace

const std::vector<std::pair<std::string, std::string>>& FontCheck::fallbackTable() {
    // 标书高频缺失字体 → Windows 自带近似
    static const std::vector<std::pair<std::string, std::string>> t = {
        {"仿宋_GB2312",   "仿宋"},
        {"楷体_GB2312",   "楷体"},
        {"宋体_GB2312",   "宋体"},
        {"方正小标宋简体", "宋体"},
        {"方正小标宋_GBK", "宋体"},
        {"华文行楷",      "微软雅黑"},
        {"华文琥珀",      "微软雅黑"},
        {"华文彩云",      "微软雅黑"},
        {"华文新魏",      "楷体"},
        {"华文仿宋",      "仿宋"},
        {"华文楷体",      "楷体"},
        {"华文中宋",      "宋体"},
    };
    return t;
}

std::vector<std::string> FontCheck::collectUsedFonts(const Document& doc) {
    std::vector<std::string> used;
    auto addFont = [&](const std::string& f) {
        if (f.empty()) return;
        if (std::find(used.begin(), used.end(), f) == used.end())
            used.push_back(f);
    };
    auto scanPara = [&](const Paragraph& p) {
        for (const auto& r : p.runs) {
            addFont(r.fontEastAsia);
            addFont(r.fontAscii);
        }
    };
    for (const auto& n : doc.body) {
        if (n.kind == Node::Kind::Paragraph) {
            scanPara(n.paragraph);
        } else {
            for (const auto& row : n.table.rows)
                for (const auto& cell : row.cells)
                    for (const auto& p : cell.paragraphs)
                        scanPara(p);
        }
    }
    return used;
}

std::vector<FontIssue> FontCheck::check(const std::vector<std::string>& used,
                                        const std::set<std::string>& installed) {
    // 预处理：系统字体名小写集合
    std::set<std::string> instLower;
    for (const auto& f : installed) instLower.insert(toLower(f));

    std::vector<FontIssue> issues;
    for (const auto& f : used) {
        if (instLower.count(toLower(f))) continue;       // 已安装
        FontIssue iss;
        iss.missing = f;
        // 回退建议（且要求回退字体确实已安装，否则无意义）
        for (const auto& [from, to] : fallbackTable()) {
            if (from == f && instLower.count(toLower(to))) {
                iss.fallback = to;
                break;
            }
        }
        issues.push_back(std::move(iss));
    }
    return issues;
}

} // namespace oc
