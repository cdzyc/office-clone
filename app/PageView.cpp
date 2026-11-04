#include "PageView.h"
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextLayout>

namespace oc::qt {

static int tw2px(uint32_t tw) { return int(tw / 15); }   // 96DPI：1440tw=96px

PageView::PageView(QWidget* parent) : QAbstractScrollArea(parent) {
    setFrameStyle(QFrame::NoFrame);
    setDocument(nullptr);
}

void PageView::setDocument(std::shared_ptr<const oc::Document> doc) {
    doc_ = std::move(doc);
    verticalScrollBar()->setValue(0);
    rebuildLayout();
    updateScrollRange();
    viewport()->update();
}

void PageView::rebuildLayout() {
    pages_.clear();
    lines_.clear();
    if (!doc_) return;

    const oc::PageSetup& pg = doc_->page;
    pageWpx_ = tw2px(pg.pageWidthTw);
    pageHpx_ = tw2px(pg.pageHeightTw);
    marginToppx_ = tw2px(pg.marginTopTw);
    marginBottompx_ = tw2px(pg.marginBottomTw);
    marginLeftpx_ = tw2px(pg.marginLeftTw);
    marginRightpx_ = tw2px(pg.marginRightTw);

    // 段落 → 行引用（M2：QTextLayout 精确断行；此处 1 段 ≈ 1 行粗估）
    for (const auto& node : doc_->body) {
        if (node.kind != oc::Node::Kind::Paragraph) continue;   // 表格 M2
        lines_.push_back(LineRef{&node.paragraph, 0});
    }

    // 行 → 页（正文区每页行数）
    const int bodyH = pageHpx_ - marginToppx_ - marginBottompx_;
    const int perPage = bodyH / lineHpx_;
    if (perPage <= 0) return;
    for (int i = 0; i < (int)lines_.size();) {
        PageInfo pi;
        pi.firstLine = i;
        pi.lineCount = std::min(perPage, (int)lines_.size() - i);
        pages_.push_back(pi);
        i += pi.lineCount;
    }
}

void PageView::updateScrollRange() {
    int totalH = 0;
    if (!pages_.empty())
        totalH = (int)pages_.size() * (pageHpx_ + gapPx_);
    verticalScrollBar()->setRange(0, qMax(0, totalH - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(lineHpx_);
    horizontalScrollBar()->setRange(0, qMax(0, pageWpx_ + 64 - viewport()->width()));
}

void PageView::resizeEvent(QResizeEvent* e) {
    QAbstractScrollArea::resizeEvent(e);
    updateScrollRange();
}

void PageView::paintEvent(QPaintEvent*) {
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), QColor(0x52, 0x54, 0x5A));

    if (!doc_ || pages_.empty()) {
        p.setPen(Qt::white);
        p.drawText(viewport()->rect(), Qt::AlignCenter,
                   tr("Ctrl+O 打开 .docx（大文件后台加载，媒体按需解码）"));
        return;
    }

    const int scrollY = verticalScrollBar()->value();
    const int scrollX = horizontalScrollBar()->value();
    const int viewH = viewport()->height();
    const int y0 = 8;   // 顶部留白

    // ---- 虚拟化核心：只画视口内的页（±1 页余量）----
    int pageHtotal = pageHpx_ + gapPx_;
    int firstVisible = qMax(0, (scrollY - y0) / pageHtotal - 1);
    int lastVisible = qMin((int)pages_.size() - 1,
                           (scrollY - y0 + viewH) / pageHtotal + 1);

    p.save();
    p.translate(32 - scrollX, y0 - scrollY);

    QFont base = font();
    for (int i = firstVisible; i <= lastVisible; ++i) {
        const PageInfo& pi = pages_[i];
        int py = i * pageHtotal;

        // 页面白底 + 阴影
        p.fillRect(QRect(6, py + 6, pageWpx_, pageHpx_), QColor(0, 0, 0, 60));
        p.fillRect(QRect(0, py, pageWpx_, pageHpx_), Qt::white);

        // 该页可见的行（视口裁剪：粗粒度按页即可）
        QFont f = base;
        for (int li = 0; li < pi.lineCount; ++li) {
            const LineRef& lr = lines_[pi.firstLine + li];
            const oc::Paragraph& para = *lr.para;

            // 段落样式（粗估：Title/Heading 用大字号）
            if (para.styleId == "Title")          f.setPointSizeF(16), f.setBold(true);
            else if (para.styleId == "Heading1")  f.setPointSizeF(14), f.setBold(true);
            else if (para.styleId == "Heading2")  f.setPointSizeF(12), f.setBold(true);
            else                                  f.setPointSizeF(10.5), f.setBold(false);
            p.setFont(f);

            int ly = py + marginToppx_ + li * lineHpx_;
            if (ly > py + pageHpx_ - marginBottompx_) break;

            // 拼接 run 文本 + 应用字符格式（首 run 格式代表整行——M2 精确化）
            QString text;
            for (const auto& run : para.runs)
                text += QString::fromStdString(run.text);
            if (!para.images.empty())
                text += QStringLiteral(" ▣[图片 %1×%2 EMU]")
                            .arg(para.images[0].cxEmu).arg(para.images[0].cyEmu);
            if (text.isEmpty()) text = QStringLiteral(" ");

            Qt::Alignment align = Qt::AlignLeft;
            if (para.align == oc::Align::Center) align = Qt::AlignHCenter;
            else if (para.align == oc::Align::Right) align = Qt::AlignRight;
            else if (para.align == oc::Align::Justify) align = Qt::AlignJustify;
            QTextLayout tl(text, f);
            tl.beginLayout();
            QTextLine line = tl.createLine();
            if (line.isValid()) {
                line.setLineWidth(pageWpx_ - marginLeftpx_ - marginRightpx_);
                line.setPosition(QPoint(0, 0));
                tl.endLayout();
                int tx = marginLeftpx_;
                if (align.testFlag(Qt::AlignHCenter))
                    tx += (pageWpx_ - marginLeftpx_ - marginRightpx_ - line.naturalTextWidth()) / 2;
                else if (align.testFlag(Qt::AlignRight))
                    tx += pageWpx_ - marginRightpx_ - marginLeftpx_ - line.naturalTextWidth();
                tl.draw(&p, QPoint(tx, ly));
            }
        }
    }
    p.restore();

    // 页码（只画可见页）
    p.setPen(QColor(0xB0, 0xB4, 0xBA));
    for (int i = firstVisible; i <= lastVisible; ++i) {
        int py = i * pageHtotal - scrollY + y0;
        p.drawText(QRect(0, py + pageHpx_ + 2, pageWpx_ + 32 - scrollX, 14),
                   Qt::AlignHCenter, QString::number(i + 1));
    }
}

} // namespace oc::qt
