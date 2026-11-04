// oc::Layout — 惰性分页布局引擎（ADR A6 落地）
// 设计：
//  1. 文本测量注入化（TextMeasurer）——Qt 层用 QFontMetrics 实现，核心可测；
//  2. 惰性：ensurePages(n) 只布局到第 n 页出现即停（首屏毫秒级）；
//  3. 脏区：编辑后 invalidate()，仅重排（全量布局本身很快；视口优先级见 ensurePages）；
//  4. M3 度量：粗分行模型（1 段 = ceil(textW / bodyW) 行）；精确断行 M4 接 QTextLayout。
#pragma once
#include "Document.h"
#include <memory>
#include <string>
#include <vector>

namespace oc {

struct TextMeasurer {
    virtual ~TextMeasurer() = default;
    // UTF-8 文本宽度（px）。缺省实现：CJK 全宽 = fontSize，ASCII 半宽。
    virtual double textWidth(const std::string& utf8) const = 0;
    virtual double fontSize() const = 0;
    virtual double lineSpacing() const = 0;               // px（含行距系数）
    // 图片显示高度（px）——EMU → px @96DPI（1px = 9525EMU）
    static double emuToPx(uint32_t emu) { return emu / 9525.0; }
};

// 缺省估算器（无 Qt 环境的核心度量；Qt 层注入精确度量）
class EstimateMeasurer : public TextMeasurer {
public:
    explicit EstimateMeasurer(double fontSizePt = 10.5) : fs_(fontSizePt) {
        cjk_ = fs_ * 96.0 / 72.0;                          // pt→px @96
        ascii_ = cjk_ / 2;
    }
    double textWidth(const std::string& utf8) const override {
        double w = 0;
        for (size_t i = 0; i < utf8.size();) {
            unsigned char c = utf8[i];
            if (c < 0x80) { w += ascii_; i += 1; }
            else if ((c >> 5) == 0x6) { w += cjk_; i += 2; }
            else if ((c >> 4) == 0xE) { w += cjk_; i += 3; }
            else if ((c >> 3) == 0x1E) { w += cjk_; i += 4; }   // emoji 等
            else ++i;                                      // 非法字节
        }
        return w;
    }
    double fontSize() const override { return fs_; }
    double lineSpacing() const override { return cjk_ * 1.4; }   // 1.4 倍行距估算
private:
    double fs_, cjk_, ascii_;
};

class DocumentLayout {
public:
    DocumentLayout(const Document& doc, TextMeasurer& m);

    // ---- 惰性布局 ----
    // 保证前 pages 页可用；返回当前可用页数（可能 < pages：文档不足）
    int ensurePages(int pages);
    int ensureAll();                                      // 全量（后台线程调用）
    int knownPages() const { return (int)pages_.size(); } // 当前已布局页数
    bool fullyLaid() const { return fullyLaid_; }

    // ---- 页查询 ----
    struct PageContent {
        int firstPara = 0;                                // 扁平段落索引
        int paraCount = 0;
        double usedHeight = 0;                            // 正文区已用高度 px
    };
    const PageContent& page(int i) const;                 // i 必须已布局
    int pageOfParagraph(size_t flatPara) const;           // 未布局段返回 -1

    // ---- 编辑后失效（段落指针可能全部失效 → 重建扁平视图 + 重排已布局部分）----
    void invalidate();

    // 页面几何（px @96）
    int pageW() const { return pageW_; }
    int pageH() const { return pageH_; }
    int bodyW() const { return pageW_ - mLeft_ - mRight_; }
    int bodyH() const { return pageH_ - mTop_ - mBottom_; }

private:
    void rebuildFlat();
    void layoutStep();                                    // 布局下一页
    const Document& doc_;
    TextMeasurer& m_;
    std::vector<const Paragraph*> flat_;
    struct PageBox { PageContent c; double restH; };
    std::vector<PageContent> pages_;
    size_t nextPara_ = 0;                                 // 下一个待布局段落
    bool fullyLaid_ = false;
    int pageW_ = 794, pageH_ = 1123;                      // A4 @96px
    int mTop_ = 96, mBottom_ = 96, mLeft_ = 120, mRight_ = 120;
};

} // namespace oc
