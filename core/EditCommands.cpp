#include "EditCommands.h"
#include <algorithm>

namespace oc {
namespace {
constexpr size_t kNoCell = (size_t)-1;
}

size_t DocumentEditor::paragraphCount() const {
    size_t n = 0;
    for (const auto& node : doc_.body) {
        if (node.kind == Node::Kind::Paragraph) ++n;
        else for (const auto& row : node.table.rows)
            for (const auto& cell : row.cells) n += cell.paragraphs.size();
    }
    return n;
}

DocumentEditor::ParaLoc DocumentEditor::locate(size_t flat) {
    ParaLoc loc;
    size_t n = 0;
    for (size_t bi = 0; bi < doc_.body.size(); ++bi) {
        Node& node = doc_.body[bi];
        if (node.kind == Node::Kind::Paragraph) {
            if (n == flat) {
                loc.node = &node; loc.p = &node.paragraph;
                loc.bodyIdx = bi; loc.cellIdx = kNoCell;
                return loc;
            }
            ++n;
        } else {
            for (size_t ri = 0; ri < node.table.rows.size(); ++ri) {
                auto& row = node.table.rows[ri];
                for (size_t ci = 0; ci < row.cells.size(); ++ci) {
                    auto& cell = row.cells[ci];
                    if (flat < n + cell.paragraphs.size()) {
                        size_t idx = flat - n;
                        loc.node = &node; loc.p = &cell.paragraphs[idx];
                        loc.bodyIdx = bi; loc.cellIdx = ci; loc.paraInCell = idx;
                        return loc;
                    }
                    n += cell.paragraphs.size();
                }
            }
        }
    }
    return loc;                                          // p == nullptr
}

Paragraph* DocumentEditor::paragraphAt(size_t flat) { return locate(flat).p; }
const Paragraph* DocumentEditor::paragraphAt(size_t flat) const {
    return const_cast<DocumentEditor*>(this)->locate(flat).p;
}

DocPos DocumentEditor::endOf(size_t flatPara, const Document&) {
    // 便捷构造：段落末位置（run/offset 由调用方结合 paragraphAt 填充）
    DocPos p;
    p.para = flatPara;
    return p;
}

std::string DocumentEditor::paragraphText(size_t flat) const {
    const Paragraph* p = paragraphAt(flat);
    if (!p) return {};
    std::string s;
    for (const auto& r : p->runs) s += r.text;
    return s;
}

bool DocumentEditor::insertText(DocPos pos, const std::string& utf8) {
    if (utf8.empty()) return false;
    ParaLoc loc = locate(pos.para);
    if (!loc.p) return false;
    Paragraph* p = loc.p;

    // 空段：新建 run
    if (p->runs.empty()) {
        TextRun r;
        r.text = utf8;
        size_t para = pos.para;
        stack_.execute(CommandStack::makeCmd(
            "insert",
            [this, para] { if (Paragraph* pp = paragraphAt(para)) pp->runs.clear(); },
            [this, para, r] { if (Paragraph* pp = paragraphAt(para)) pp->runs = {r}; }));
        return true;
    }
    if (pos.run >= p->runs.size()) return false;
    if (pos.off > p->runs[pos.run].text.size()) return false;

    size_t para = pos.para, run = pos.run, off = pos.off;
    stack_.execute(CommandStack::makeCmd(
        "insert",
        [this, para, run, off, len = utf8.size()] {
            if (Paragraph* pp = paragraphAt(para)) {
                std::string& t = pp->runs[run].text;
                t.erase(off, len);
            }
        },
        [this, para, run, off, text = utf8] {
            if (Paragraph* pp = paragraphAt(para)) {
                std::string& t = pp->runs[run].text;
                t.insert(off, text);
            }
        }));
    return true;
}

bool DocumentEditor::deleteRange(DocPos from, DocPos to) {
    if (!(from < to)) return false;

    // ---- 跨段删除（M2.6）：同 body 区段 或 同 cell ----
    if (from.para != to.para) {
        ParaLoc f = locate(from.para), t = locate(to.para);
        if (!f.p || !t.p) return false;
        bool fBody = f.cellIdx == kNoCell, tBody = t.cellIdx == kNoCell;
        if (fBody != tBody) return false;                 // 跨结构（body↔cell）M3 扩展
        if (!fBody && (f.bodyIdx != t.bodyIdx || f.cellIdx != t.cellIdx))
            return false;                                 // 跨 cell M3 扩展
        auto validPos = [](const DocPos& p, const Paragraph* pp) {
            if (pp->runs.empty()) return p.run == 0 && p.off == 0;
            return p.run < pp->runs.size() && p.off <= pp->runs[p.run].text.size();
        };
        if (!validPos(from, f.p) || !validPos(to, t.p)) return false;

        // 计算 from 段前缀 runs 与 to 段后缀 runs
        RunList head, tail;
        for (size_t i = 0; i <= from.run && i < f.p->runs.size(); ++i) {
            head.push_back(f.p->runs[i]);
            if (i == from.run) head.back().text = f.p->runs[i].text.substr(0, from.off);
        }
        if (f.p->runs.empty()) head.clear();
        for (size_t i = to.run; i < t.p->runs.size(); ++i) {
            TextRun r = t.p->runs[i];
            if (i == to.run) r.text = t.p->runs[i].text.substr(to.off);
            if (!r.text.empty()) tail.push_back(r);
        }
        Paragraph merged;
        merged.runs = head;
        merged.runs.insert(merged.runs.end(), tail.begin(), tail.end());
        merged.align = f.p->align;
        merged.styleId = f.p->styleId;

        if (fBody) {
            // 快照 [f.bodyIdx, t.bodyIdx] 区间，替换为单节点
            size_t fb = f.bodyIdx, tb = t.bodyIdx;
            std::vector<Node> snapshot(doc_.body.begin() + (long)fb,
                                       doc_.body.begin() + (long)tb + 1);
            Node m{Node::Kind::Paragraph, merged, Table{}};
            auto redoFn = [this, fb, tb, m]() {
                doc_.body.erase(doc_.body.begin() + (long)fb,
                                doc_.body.begin() + (long)tb + 1);
                doc_.body.insert(doc_.body.begin() + (long)fb, m);
            };
            auto undoFn = [this, fb, tb, snapshot]() {
                doc_.body.erase(doc_.body.begin() + (long)fb);
                doc_.body.insert(doc_.body.begin() + (long)fb,
                                 snapshot.begin(), snapshot.end());
            };
            stack_.execute(CommandStack::makeCmd("delete-para", undoFn, redoFn));
            return true;
        }
        // cell 内跨段（M2.6）：整 cell 段落快照
        size_t bi = f.bodyIdx, ci = f.cellIdx;
        auto getParas = [this, bi, ci]() -> std::vector<Paragraph>* {
            for (auto& row : doc_.body[bi].table.rows)
                if (ci < row.cells.size()) return &row.cells[ci].paragraphs;
            return nullptr;
        };
        std::vector<Paragraph>* ps = getParas();
        if (!ps) return false;
        std::vector<Paragraph> snapshot = *ps;
        auto redoFn = [this, bi, ci, f, t, merged]() {
            auto* pp = const_cast<std::vector<Paragraph>*>([&]() -> const std::vector<Paragraph>* {
                for (auto& row : doc_.body[bi].table.rows)
                    if (ci < row.cells.size()) return &row.cells[ci].paragraphs;
                return nullptr;
            }());
            if (!pp) return;
            // 重算扁平段落在 cell 内的索引（快照恢复后结构可能变化——按快照语义重放）
            size_t i0 = f.paraInCell, i1 = t.paraInCell;
            if (i1 >= pp->size()) i1 = pp->size() ? pp->size() - 1 : 0;
            pp->erase(pp->begin() + (long)i0, pp->begin() + (long)i1 + 1);
            pp->insert(pp->begin() + (long)i0, merged);
        };
        auto undoFn = [this, bi, ci, snapshot]() {
            for (auto& row : doc_.body[bi].table.rows)
                if (ci < row.cells.size()) { row.cells[ci].paragraphs = snapshot; return; }
        };
        stack_.execute(CommandStack::makeCmd("delete-cell-para", undoFn, redoFn));
        return true;
    }

    // ---- 同段删除（原实现）----
    ParaLoc loc = locate(from.para);
    if (!loc.p) return false;
    Paragraph* p = loc.p;
    if (from.run >= p->runs.size() || to.run >= p->runs.size()) return false;
    if (from.off > p->runs[from.run].text.size()) return false;
    if (to.off > p->runs[to.run].text.size()) return false;

    std::string head = p->runs[from.run].text.substr(0, from.off);
    std::string tail = p->runs[to.run].text.substr(to.off);
    RunList after = p->runs;
    after[from.run].text = head + tail;                   // 保留 from run 格式
    if (from.run != to.run)
        after.erase(after.begin() + (long)from.run + 1,
                    after.begin() + (long)to.run + 1);
    RunList oldRuns = p->runs, newRuns = after;
    size_t para = from.para;
    p->runs = newRuns;
    stack_.execute(CommandStack::makeCmd(
        "delete",
        [this, para, oldRuns] { if (Paragraph* pp = paragraphAt(para)) pp->runs = oldRuns; },
        [this, para, newRuns] { if (Paragraph* pp = paragraphAt(para)) pp->runs = newRuns; }));
    return true;
}

bool DocumentEditor::splitParagraph(DocPos pos) {
    ParaLoc loc = locate(pos.para);
    if (!loc.p) return false;
    Paragraph* p = loc.p;
    if (p->runs.empty()) {
        // 空段回车：body 段落 → 插入空节点；cell 段落 → 追加空段
        // （M2 简化处理）
    }
    if (pos.run >= p->runs.size()) return false;
    if (pos.off > p->runs[pos.run].text.size()) return false;

    // 计算前/后两半 runs
    RunList before, after;
    for (size_t i = 0; i < p->runs.size(); ++i) {
        const auto& r = p->runs[i];
        if (i < pos.run) before.push_back(r);
        else if (i > pos.run) after.push_back(r);
        else {
            before.push_back(r);
            before.back().text = r.text.substr(0, pos.off);
            TextRun tail = r;
            tail.text = r.text.substr(pos.off);
            if (!tail.text.empty()) after.insert(after.begin(), tail);
        }
    }

    size_t para = pos.para;
    if (loc.cellIdx == kNoCell) {
        // body 段落：当前节点后插入新段落节点
        size_t bodyIdx = loc.bodyIdx;
        Node splitNode{Node::Kind::Paragraph, Paragraph{}, Table{}};
        splitNode.paragraph.runs = after;
        Node newNode = splitNode;

        auto doSplit = [this, bodyIdx, before, newNode]() {
            doc_.body[bodyIdx].paragraph.runs = before;
            doc_.body.insert(doc_.body.begin() + (long)bodyIdx + 1, newNode);
        };
        auto doMerge = [this, bodyIdx, para]() {
            // 注意：undo 时扁平索引已变化——但 bodyIdx 不变（插入在其后）
            Paragraph& dst = doc_.body[bodyIdx].paragraph;
            Paragraph& src = doc_.body[bodyIdx + 1].paragraph;
            dst.runs.insert(dst.runs.end(), src.runs.begin(), src.runs.end());
            doc_.body.erase(doc_.body.begin() + (long)bodyIdx + 1);
            (void)para;
        };
        stack_.execute(CommandStack::makeCmd("split", doMerge, doSplit));
        return true;
    }
    // cell 段落：cell 内追加
    size_t bodyIdx = loc.bodyIdx, cellIdx = loc.cellIdx, pic = loc.paraInCell;
    auto doSplitCell = [this, bodyIdx, cellIdx, pic, before, after]() {
        auto* ps = const_cast<std::vector<Paragraph>*>([&]() -> const std::vector<Paragraph>* {
            for (auto& row : doc_.body[bodyIdx].table.rows)
                if (cellIdx < row.cells.size()) return &row.cells[cellIdx].paragraphs;
            return nullptr;
        }());
        if (!ps) return;
        (*ps)[pic].runs = before;
        Paragraph np; np.runs = after;
        ps->insert(ps->begin() + (long)pic + 1, np);
    };
    auto doMergeCell = [this, bodyIdx, cellIdx, pic]() {
        for (auto& row : doc_.body[bodyIdx].table.rows) {
            if (cellIdx < row.cells.size()) {
                auto& ps = row.cells[cellIdx].paragraphs;
                ps[pic].runs.insert(ps[pic].runs.end(),
                                    ps[pic + 1].runs.begin(), ps[pic + 1].runs.end());
                ps.erase(ps.begin() + (long)pic + 1);
                return;
            }
        }
    };
    stack_.execute(CommandStack::makeCmd("split-cell", doMergeCell, doSplitCell));
    return true;
}

bool DocumentEditor::setBold(DocPos from, DocPos to, bool val) {
    return formatRange(from, to,
        [val](TextRun& r) { r.bold = val; }, "bold");
}

bool DocumentEditor::setItalic(DocPos from, DocPos to, bool val) {
    return formatRange(from, to,
        [val](TextRun& r) { r.italic = val; }, "italic");
}

bool DocumentEditor::setUnderline(DocPos from, DocPos to, bool val) {
    return formatRange(from, to,
        [val](TextRun& r) { r.underline = val; }, "underline");
}

bool DocumentEditor::setFontSize(DocPos from, DocPos to, double pt) {
    if (pt < 4 || pt > 200) return false;
    return formatRange(from, to,
        [pt](TextRun& r) { r.fontSizePt = pt; }, "fontsize");
}

bool DocumentEditor::setFontName(DocPos from, DocPos to, const std::string& eastAsia,
                                 const std::string& ascii) {
    return formatRange(from, to,
        [&](TextRun& r) { r.fontEastAsia = eastAsia; r.fontAscii = ascii; }, "fontname");
}

// 字符级格式通用实现：选区与各 run 求交，交集副本应用变更，相邻同格式合并。
// 全部走 CommandStack（段落快照 undo）。
bool DocumentEditor::formatRange(DocPos from, DocPos to,
                                 const std::function<void(TextRun&)>& mutate,
                                 const char* name) {
    if (from.para != to.para || to < from) return false;
    ParaLoc loc = locate(from.para);
    if (!loc.p) return false;
    Paragraph* p = loc.p;
    if (p->runs.empty()) return from.run == 0 && to.run == 0 && from.off == 0 && to.off == 0;

    // 字符区间边界（UTF-8 字节偏移 → 累计字符数）
    auto runStartByte = [&](size_t idx) {
        size_t s = 0;
        for (size_t i = 0; i < idx; ++i) s += p->runs[i].text.size();
        return s;
    };
    size_t selStart = runStartByte(from.run) + from.off;
    size_t selEnd   = runStartByte(to.run) + to.off;

    RunList after;
    size_t cur = 0;
    for (const auto& r : p->runs) {
        size_t rStart = cur, rEnd = cur + r.text.size();
        // 三段：前缀 / 交集 / 后缀
        if (rEnd <= selStart || rStart >= selEnd) {
            after.push_back(r);                          // 与选区无交
        } else {
            if (rStart < selStart) {
                TextRun head = r;
                head.text = r.text.substr(0, selStart - rStart);
                after.push_back(head);
            }
            {
                TextRun mid = r;
                size_t is = std::max(rStart, selStart), ie = std::min(rEnd, selEnd);
                mid.text = r.text.substr(is - rStart, ie - is);
                mutate(mid);
                after.push_back(mid);
            }
            if (rEnd > selEnd) {
                TextRun tail = r;
                tail.text = r.text.substr(selEnd - rStart);
                after.push_back(tail);
            }
        }
        cur = rEnd;
    }
    // 合并相邻同格式（防 runs 碎片化）
    RunList merged;
    for (auto& r : after) {
        if (!merged.empty() && r.text.empty()) continue;
        if (r.text.empty()) continue;
        if (!merged.empty()) {
            TextRun& last = merged.back();
            if (last.bold == r.bold && last.italic == r.italic &&
                last.underline == r.underline && last.fontSizePt == r.fontSizePt &&
                last.styleId == r.styleId && last.fontAscii == r.fontAscii &&
                last.fontEastAsia == r.fontEastAsia) {
                last.text += r.text;
                continue;
            }
        }
        merged.push_back(r);
    }

    RunList oldRuns = p->runs, newRuns = merged;
    size_t para = from.para;
    stack_.execute(CommandStack::makeCmd(
        name,
        [this, para, oldRuns] { if (Paragraph* pp = paragraphAt(para)) pp->runs = oldRuns; },
        [this, para, newRuns] { if (Paragraph* pp = paragraphAt(para)) pp->runs = newRuns; }));
    return true;
}

bool DocumentEditor::setParaStyle(size_t flatPara, const std::string& styleId) {
    ParaLoc loc = locate(flatPara);
    if (!loc.p) return false;
    std::string old = loc.p->styleId;
    size_t para = flatPara;
    stack_.execute(CommandStack::makeCmd(
        "parastyle",
        [this, para, old] { if (Paragraph* pp = paragraphAt(para)) pp->styleId = old; },
        [this, para, styleId] { if (Paragraph* pp = paragraphAt(para)) pp->styleId = styleId; }));
    return true;
}

bool DocumentEditor::setAlign(size_t flatPara, Align a) {
    ParaLoc loc = locate(flatPara);
    if (!loc.p) return false;
    Align old = loc.p->align;
    size_t para = flatPara;
    stack_.execute(CommandStack::makeCmd(
        "align",
        [this, para, old] { if (Paragraph* pp = paragraphAt(para)) pp->align = old; },
        [this, para, a] { if (Paragraph* pp = paragraphAt(para)) pp->align = a; }));
    return true;
}

ClipboardFragment DocumentEditor::copyRange(DocPos from, DocPos to) {
    ClipboardFragment frag;
    if (!(from < to)) return frag;
    ParaLoc f = locate(from.para), t = locate(to.para);
    if (!f.p || !t.p) return frag;
    auto validPos = [](const DocPos& p, const Paragraph* pp) {
        if (pp->runs.empty()) return p.run == 0 && p.off == 0;
        return p.run < pp->runs.size() && p.off <= pp->runs[p.run].text.size();
    };
    if (!validPos(from, f.p) || !validPos(to, t.p)) return frag;

    // 同段：截取 [from, to) 的 runs
    if (from.para == to.para) {
        Paragraph cut;
        for (size_t i = from.run; i <= to.run; ++i) {
            TextRun r = f.p->runs[i];
            if (i == from.run && i == to.run) r.text = r.text.substr(from.off, to.off - from.off);
            else if (i == from.run) r.text = r.text.substr(from.off);
            else if (i == to.run)  r.text = r.text.substr(0, to.off);
            if (!r.text.empty()) cut.runs.push_back(r);
        }
        cut.align = f.p->align;
        frag.paras.push_back(std::move(cut));
        return frag;
    }

    // 跨段：首段（from 之后）、中间整段、末段（to 之前）
    Paragraph first;
    for (size_t i = from.run; i < f.p->runs.size(); ++i) {
        TextRun r = f.p->runs[i];
        if (i == from.run) r.text = r.text.substr(from.off);
        if (!r.text.empty()) first.runs.push_back(r);
    }
    first.align = f.p->align;
    frag.paras.push_back(std::move(first));

    for (size_t fp = from.para + 1; fp < to.para; ++fp) {
        Paragraph* mid = paragraphAt(fp);
        if (mid && !mid->runs.empty())
            frag.paras.push_back(*mid);
    }

    Paragraph last;
    if (!t.p->runs.empty()) {
        TextRun r = t.p->runs[to.run];
        r.text = r.text.substr(0, to.off);
        if (!r.text.empty()) last.runs.push_back(r);
    }
    last.align = t.p->align;
    if (!last.runs.empty() || frag.paras.empty())
        frag.paras.push_back(std::move(last));
    return frag;
}

bool DocumentEditor::paste(DocPos pos, const ClipboardFragment& frag) {
    if (frag.empty()) return false;
    ParaLoc loc = locate(pos.para);
    if (!loc.p) return false;
    Paragraph* p = loc.p;

    // ---- 单段片段：pos 处插入 runs（拆 run 保留前后格式）----
    if (frag.paras.size() == 1) {
        bool emptyPara = p->runs.empty();
        if (!emptyPara) {
            if (pos.run >= p->runs.size()) return false;
            if (pos.off > p->runs[pos.run].text.size()) return false;
        } else if (pos.run != 0 || pos.off != 0) {
            return false;
        }

        RunList before, after, ins = frag.paras[0].runs;
        for (size_t i = 0; i < p->runs.size(); ++i) {
            const auto& r = p->runs[i];
            if (i < pos.run) before.push_back(r);
            else if (i > pos.run) after.push_back(r);
            else {
                TextRun head = r, tail = r;
                head.text = r.text.substr(0, pos.off);
                tail.text = r.text.substr(pos.off);
                if (!head.text.empty()) before.push_back(head);
                if (!tail.text.empty()) after.push_back(tail);
            }
        }
        // 合并：before + ins + after
        RunList merged = before;
        merged.insert(merged.end(), ins.begin(), ins.end());
        merged.insert(merged.end(), after.begin(), after.end());
        if (merged.empty()) merged = ins;

        RunList oldRuns = p->runs, newRuns = merged;
        size_t para = pos.para;
        stack_.execute(CommandStack::makeCmd(
            "paste",
            [this, para, oldRuns] { if (Paragraph* pp = paragraphAt(para)) pp->runs = oldRuns; },
            [this, para, newRuns] { if (Paragraph* pp = paragraphAt(para)) pp->runs = newRuns; }));
        return true;
    }

    // ---- 多段片段：拆当前段，中间插入（仅 body 段落目标）----
    if (loc.cellIdx != kNoCell) return false;            // cell 内多段粘贴 M3 扩展
    if (pos.run >= p->runs.size()) return false;
    if (pos.off > p->runs[pos.run].text.size()) return false;

    RunList before, after;
    for (size_t i = 0; i < p->runs.size(); ++i) {
        const auto& r = p->runs[i];
        if (i < pos.run) before.push_back(r);
        else if (i > pos.run) after.push_back(r);
        else {
            TextRun head = r, tail = r;
            head.text = r.text.substr(0, pos.off);
            tail.text = r.text.substr(pos.off);
            if (!head.text.empty()) before.push_back(head);
            if (!tail.text.empty()) after.push_back(tail);
        }
    }
    // 新节点序列：before 段 + frag 各段 + after 段
    std::vector<Node> replacement;
    if (!before.empty()) {
        Node n{Node::Kind::Paragraph, *p, Table{}};
        n.paragraph.runs = before;
        replacement.push_back(std::move(n));
    }
    for (const auto& fp : frag.paras) {
        Node n{Node::Kind::Paragraph, fp, Table{}};
        replacement.push_back(std::move(n));
    }
    if (!after.empty()) {
        Node n{Node::Kind::Paragraph, *p, Table{}};
        n.paragraph.runs = after;
        replacement.push_back(std::move(n));
    }
    if (replacement.empty())
        replacement.emplace_back(Node::Kind::Paragraph, Paragraph{}, Table{});

    size_t bodyIdx = loc.bodyIdx;
    std::vector<Node> snapshot(doc_.body.begin() + (long)bodyIdx,
                               doc_.body.begin() + (long)bodyIdx + 1);
    auto redoFn = [this, bodyIdx, replacement]() {
        doc_.body.erase(doc_.body.begin() + (long)bodyIdx);
        doc_.body.insert(doc_.body.begin() + (long)bodyIdx,
                         replacement.begin(), replacement.end());
    };
    auto undoFn = [this, bodyIdx, snapshot, n = replacement.size()]() {
        doc_.body.erase(doc_.body.begin() + (long)bodyIdx,
                        doc_.body.begin() + (long)bodyIdx + (long)n);
        doc_.body.insert(doc_.body.begin() + (long)bodyIdx,
                         snapshot.begin(), snapshot.end());
    };
    stack_.execute(CommandStack::makeCmd("paste-multi", undoFn, redoFn));
    return true;
}

} // namespace oc
