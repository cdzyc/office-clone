#include "Layout.h"
#include <cmath>
#include <cstdlib>

namespace oc {
namespace {
int tw2px(uint32_t tw) { return (int)(tw / 15); }         // 1440tw = 96px
}

DocumentLayout::DocumentLayout(const Document& doc, TextMeasurer& m)
    : doc_(doc), m_(m) {
    pageW_ = tw2px(doc.page.pageWidthTw);
    pageH_ = tw2px(doc.page.pageHeightTw);
    mTop_ = tw2px(doc.page.marginTopTw);
    mBottom_ = tw2px(doc.page.marginBottomTw);
    mLeft_ = tw2px(doc.page.marginLeftTw);
    mRight_ = tw2px(doc.page.marginRightTw);
    rebuildFlat();
}

void DocumentLayout::rebuildFlat() {
    flat_.clear();
    for (const auto& node : doc_.body) {
        if (node.kind == Node::Kind::Paragraph) {
            flat_.push_back(&node.paragraph);
        } else {
            for (const auto& row : node.table.rows)
                for (const auto& cell : row.cells)
                    for (const auto& p : cell.paragraphs)
                        flat_.push_back(&p);
        }
    }
}

void DocumentLayout::invalidate() {
    rebuildFlat();
    pages_.clear();
    nextPara_ = 0;
    fullyLaid_ = false;
}

void DocumentLayout::layoutStep() {
    // 布局一页：从 nextPara_ 累计段落高度直到装不下
    PageContent pc;
    pc.firstPara = (int)nextPara_;
    double avail = bodyH();
    double used = 0;

    while (nextPara_ < flat_.size()) {
        const Paragraph* p = flat_[nextPara_];

        // 段落高度 = 文本行数 × 行距 + 图片高度 + 段前后距
        std::string text;
        for (const auto& r : p->runs) text += r.text;
        double textW = m_.textWidth(text);
        int lines = text.empty() ? 1 : (int)std::ceil(textW / std::max(1.0, (double)bodyW()));
        double paraH = lines * m_.lineSpacing();
        for (const auto& img : p->images)
            paraH += TextMeasurer::emuToPx(img.cyEmu);
        // 段间距（spacing 单位 1/16pt? OOXML twentieths → px）
        paraH += p->spacingBeforePt16 / 20.0 * 96.0 / 72.0;
        paraH += p->spacingAfterPt16 / 20.0 * 96.0 / 72.0;

        if (used + paraH > avail && pc.paraCount > 0)
            break;                                        // 本页装不下 → 换页
        // 段落跨页：简化为整段下移（不可分割段）；跨段 M4 优化
        used += paraH;
        ++pc.paraCount;
        ++nextPara_;
        // 图片超大保护：单段超页高也必须放置（防死循环）
        if (used >= avail) break;
    }

    pc.usedHeight = used;
    pages_.push_back(pc);
    if (nextPara_ >= flat_.size()) fullyLaid_ = true;
    // 空文档兜底
    if (flat_.empty()) fullyLaid_ = true;
}

int DocumentLayout::ensurePages(int want) {
    if (flat_.empty() && pages_.empty()) { layoutStep(); return (int)pages_.size(); }
    while ((int)pages_.size() < want && !fullyLaid_)
        layoutStep();
    return (int)pages_.size();
}

int DocumentLayout::ensureAll() {
    while (!fullyLaid_)
        layoutStep();
    if (pages_.empty()) layoutStep();
    return (int)pages_.size();
}

const DocumentLayout::PageContent& DocumentLayout::page(int i) const {
    static const PageContent kEmpty{};
    if (i < 0 || i >= (int)pages_.size()) return kEmpty;
    return pages_[(size_t)i];
}

int DocumentLayout::pageOfParagraph(size_t flatPara) const {
    for (size_t pi = 0; pi < pages_.size(); ++pi) {
        const auto& pc = pages_[pi];
        if (flatPara >= (size_t)pc.firstPara &&
            flatPara < (size_t)(pc.firstPara + pc.paraCount))
            return (int)pi;
    }
    return -1;
}

} // namespace oc
