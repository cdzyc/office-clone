// oc::qt::PageView — 分页虚拟化渲染骨架
// 核心思想：无论文档多少页，paintEvent 只遍历"视口 ± 1 页"；
// 布局为粗估分页（M2 替换为真正的惰性排版引擎 LayoutEngine）。
#pragma once
#include "../core/Document.h"
#include <QAbstractScrollArea>
#include <memory>

namespace oc::qt {

class PageView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PageView(QWidget* parent = nullptr);

    void setDocument(std::shared_ptr<const oc::Document> doc);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    // ---- 粗估布局（M2：替换为 LayoutEngine 精确分页）----
    struct LineRef {           // 一"逻辑行" = 段落的一部分（简化为整段）
        const oc::Paragraph* para = nullptr;
        int lineInPara = 0;
    };
    void rebuildLayout();      // 段落 → 行 → 页（粗估）
    void updateScrollRange();

    std::shared_ptr<const oc::Document> doc_;

    // 布局参数（twips → px @96DPI：px = tw / 15）
    int pageWpx_ = 0, pageHpx_ = 0;
    int marginToppx_ = 0, marginBottompx_ = 0;
    int marginLeftpx_ = 0, marginRightpx_ = 0;
    int lineHpx_ = 22;         // M2：由字体测量决定
    int gapPx_ = 24;           // 页间距

    // 粗分页结果
    struct PageInfo { int firstLine = 0, lineCount = 0; };
    std::vector<PageInfo> pages_;
    std::vector<LineRef> lines_;
};

} // namespace oc::qt
