#include "PageView.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QBrush>
#include <QColor>
#include <QTextCharFormat>
#include <map>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextOption>
#include <QRegularExpression>
#include <algorithm>

namespace oc::qt {
namespace {

// 段落字体（样式区分：Title/Heading 大字号加粗）
QFont fontFor(const oc::Paragraph& p, const QFont& base) {
    QFont f = base;
    if (p.styleId == "Title")          { f.setPointSizeF(17); f.setBold(true); }
    else if (p.styleId == "Heading1")  { f.setPointSizeF(15); f.setBold(true); }
    else if (p.styleId == "Heading2")  { f.setPointSizeF(13); f.setBold(true); }
    else if (p.styleId == "Heading3")  { f.setPointSizeF(11.5); f.setBold(true); }
    else                               { f.setPointSizeF(10.5); }
    return f;
}

Qt::Alignment alignFor(const oc::Paragraph& p) {
    switch (p.align) {
    case oc::Align::Center:  return Qt::AlignHCenter;
    case oc::Align::Right:   return Qt::AlignRight;
    case oc::Align::Justify: return Qt::AlignJustify;
    default:                 return Qt::AlignLeft;
    }
}

// run 富格式 → QTextLayout::FormatRange 列表（字符级：B/I/U/字号/字体真实可见）
QVector<QTextLayout::FormatRange> formatsFor(const oc::Paragraph& p, const QFont& base) {
    QVector<QTextLayout::FormatRange> ranges;
    ranges.reserve((int)p.runs.size());
    int charPos = 0;
    bool any = false;
    for (const auto& r : p.runs) {
        int chars = (int)QString::fromStdString(r.text).size();
        if (chars == 0) continue;
        QTextCharFormat fmt;
        bool diff = false;
        QFont f = base;
        if (r.bold)      { f.setBold(true); diff = true; }
        if (r.italic)    { f.setItalic(true); diff = true; }
        if (r.underline) { f.setUnderline(true); diff = true; }
        if (r.strike)    { f.setStrikeOut(true); diff = true; }
        if (!r.color.empty()) {
            fmt.setForeground(QBrush(QColor(QString::fromStdString("#" + r.color))));
            diff = true;
        }
        if (!r.highlight.empty()) {
            static const std::map<std::string, const char*> hlColors = {
                {"yellow", "#FFE100"}, {"bright_green", "#00FF00"}, {"green", "#00B050"},
                {"cyan", "#00FFFF"}, {"magenta", "#FF00FF"}, {"blue", "#00B0F0"},
                {"red", "#FF0000"}, {"darkBlue", "#002060"}, {"darkYellow", "#808000"},
                {"darkGreen", "#375623"}, {"darkCyan", "#00838F"}, {"darkMagenta", "#7030A0"},
                {"lightGray", "#C0C0C0"}, {"darkGray", "#808080"}, {"black", "#000000"},
                {"white", "#FFFFFF"},
            };
            auto it = hlColors.find(r.highlight);
            if (it != hlColors.end()) {
                fmt.setBackground(QBrush(QColor(it->second)));
                diff = true;
            }
        }
        if (r.fontSizePt > 0) { f.setPointSizeF(r.fontSizePt); diff = true; }
        if (!r.fontEastAsia.empty()) { f.setFamily(QString::fromStdString(r.fontEastAsia)); diff = true; }
        else if (!r.fontAscii.empty()) { f.setFamily(QString::fromStdString(r.fontAscii)); diff = true; }
        if (diff) {
            fmt.setFont(f);
            fmt.setFontFamilies(QStringList() << f.family());
            QTextLayout::FormatRange fr;
            fr.start = charPos;
            fr.length = chars;
            fr.format = fmt;
            ranges.append(fr);
            any = true;
        }
        charPos += chars;
    }
    if (!any) ranges.clear();
    return ranges;
}

} // namespace

PageView::PageView(QWidget* parent) : QAbstractScrollArea(parent) {
    setFrameStyle(QFrame::NoFrame);
    setFont(QFont(QStringLiteral("Microsoft YaHei")));
    viewport()->setCursor(Qt::IBeamCursor);
    setFocusPolicy(Qt::StrongFocus);                 // 接收键盘
    setAttribute(Qt::WA_InputMethodEnabled, true);   // 中文输入法
    blinkTimer_ = startTimer(530);                   // 光标闪烁
    setDocument(nullptr);
}

void PageView::setDocument(std::shared_ptr<const oc::Document> doc) {
    doc_ = std::move(doc);
    cursor_ = DocPos{};
    anchor_ = DocPos{};
    verticalScrollBar()->setValue(0);
    rebuildLayout();
    viewport()->update();
}

void PageView::attachEditor(oc::DocumentEditor* ed, oc::CommandStack* cs) {
    ed_ = ed;
    cs_ = cs;
}

void PageView::attachImageCache(oc::ImageCache* cache) {
    cache_ = cache;
    rebuildLayout();
    viewport()->update();
}

// ---------------- 布局 ----------------

void PageView::rebuildLayout() {
    boxes_.clear();
    tables_.clear();
    docHeight_ = topPad_;
    if (!doc_) {
        updateScrollRange();
        return;
    }
    // 文档流布局：遍历 body 节点（段落 + 表格）。flat 编址与 DocumentEditor 一致。
    const qreal textW = qreal(std::max(160, viewport()->width() - 2 * textMarginX_));
    size_t flat = 0;

    auto layoutParagraph = [&](const oc::Paragraph* p, size_t flatIdx, qreal x, qreal width,
                               qreal y, ParaBox& b) -> qreal {
        b.flat = (int)flatIdx;
        b.x = x;
        b.font = fontFor(*p, font());
        QString text;
        for (const auto& r : p->runs) text += QString::fromStdString(r.text);
        if (!p->images.empty()) text += QStringLiteral(" ▣图片%1 ").arg(p->images.size());
        // 缩进（twips→px@96：/15）+ 行距倍数（w:line 240=单倍）
        const qreal indL = p->indLeftTw / 15.0, indR = p->indRightTw / 15.0;
        const qreal indFL = p->indFirstLineTw / 15.0;
        b.textX = x + indL;
        // ⚠️ 行宽下限保护：缩进再大也不允许 ≤40px（防负宽死循环——真机战报 #5 崩溃根因）
        b.textW = std::max(qreal(40), width - indL - indR);
        b.firstLineIndent = indFL;
        b.lineFactor = p->lineSpacing > 0 ? (p->lineSpacing / 240.0) : 1.0;

        b.layout = std::make_unique<QTextLayout>(text, b.font);
        b.layout->setTextOption(QTextOption(alignFor(*p)));
        b.layout->setFormats(formatsFor(*p, b.font));
        b.layout->beginLayout();
        forever {
            QTextLine line = b.layout->createLine();
            if (!line.isValid()) break;
            line.setLineWidth(b.textW);
            b.lines.append(line);
        }
        b.layout->endLayout();
        // 图片区（文本前置）：等比缩放至段宽内，真实尺寸 EMU→px(96dpi)
        for (const auto& im : p->images) {
            qreal w = im.cxEmu / 9525.0, h = im.cyEmu / 9525.0;
            if (w > width) { h = h * width / w; w = width; }
            if (w < 1 || h < 1) { w = 120; h = 90; }
            ParaBox::Img bi; bi.relId = QString::fromStdString(im.relId);
            bi.w = w; bi.h = h;
            b.imgs.append(bi);
            b.imgBlockH += h + 4;
        }
        // ⚠️ 逐行纵向定位（图片区之下；修复真机战报 #2：行叠画）
        qreal lineY = b.imgBlockH;
        const qreal factor = b.lineFactor > 0.01 ? b.lineFactor : 1.0;
        bool firstLine = true;
        for (auto& l : b.lines) {
            const qreal lx = firstLine ? std::max(qreal(0), b.firstLineIndent) : 0;
            l.setPosition(QPointF(lx, lineY));
            lineY += l.height() * factor + lineGap_;
            firstLine = false;
        }
        b.h = qMax(lineY - lineGap_, 18.0) + paraGap_;
        b.y = y;
        return b.h;
    };

    for (const auto& node : doc_->body) {
        if (node.kind == oc::Node::Kind::Paragraph) {
            ParaBox b;
            layoutParagraph(&node.paragraph, flat, textMarginX_, textW, docHeight_, b);
            docHeight_ += b.h;
            boxes_.push_back(std::move(b));
            ++flat;
        } else {
            // ---- 表格（真机战报 #3：必须渲染）----
            const oc::Table& t = node.table;
            TableBox tb;
            tb.w = textW;
            // 列宽：按 colWidthsDxa 比例（缺失时均分）
            qreal dxaSum = 0;
            for (uint32_t w : t.colWidthsDxa) dxaSum += w;
            size_t cols = t.colWidthsDxa.size();
            for (const auto& row : t.rows) cols = std::max(cols, row.cells.size());
            if (cols == 0) { ++flat; continue; }
            // 列几何：正确累计起点（colX[c] = 前列右缘）
            {
                qreal x = textMarginX_;
                for (size_t c = 0; c < cols; ++c) {
                    qreal w = (t.colWidthsDxa.size() == cols)
                                  ? textW * t.colWidthsDxa[c] / (dxaSum > 0 ? dxaSum : 1)
                                  : textW / cols;
                    tb.colX.push_back(x);
                    tb.colW.push_back(w);
                    x += w;
                }
            }
            // 行高 = 各 cell 段落最高者 + padding
            const int pad = 4;
            for (const auto& row : t.rows) {
                qreal rowH = 22;
                qreal cellY = docHeight_ + pad;
                qreal colStart = 0;
                for (size_t c = 0; c < row.cells.size() && c < cols; ++c) {
                    const oc::TableCell& cell = row.cells[c];
                    qreal colSpanW = tb.colW[c];
                    qreal y = cellY;
                    for (const auto& p : cell.paragraphs) {
                        ParaBox b;
                        qreal usedW = std::max(40.0, tb.colW[c] - 2 * pad);
                        layoutParagraph(&p, flat, tb.colX[c] + pad, usedW, y, b);
                        y += b.h;
                        boxes_.push_back(std::move(b));
                        tb.allParas.push_back((int)flat);
                        if (&row == &t.rows.front())
                            tb.headerParas.push_back((int)flat);
                        ++flat;
                    }
                    rowH = std::max(rowH, y - docHeight_ + pad);
                }
                docHeight_ = std::max(docHeight_, cellY - pad + rowH);
                (void)colStart;
            }
            tb.h = docHeight_ - tb.y + 6;
            tables_.push_back(std::move(tb));
            docHeight_ += 10;
        }
    }
    docHeight_ += topPad_;
    updateScrollRange();
}

void PageView::updateScrollRange() {
    verticalScrollBar()->setRange(0, qMax(0, int(docHeight_) - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(24);
}

void PageView::resizeEvent(QResizeEvent* e) {
    QAbstractScrollArea::resizeEvent(e);
    rebuildLayout();
    updateScrollRange();
}

// ---------------- 坐标/命中 ----------------

int PageView::hitPara(qreal docY) const {
    for (int i = 0; i < (int)boxes_.size(); ++i)
        if (docY >= boxes_[(size_t)i].y && docY < boxes_[(size_t)i].y + boxes_[(size_t)i].h)
            return i;
    return docY <= topPad_ ? 0 : (int)boxes_.size() - 1;
}

DocPos PageView::hitPos(const QPointF& docPt) const {
    DocPos pos;
    int bi = hitPara(docPt.y());
    if (bi < 0 || bi >= (int)boxes_.size()) return pos;
    const ParaBox& b = boxes_[(size_t)bi];
    // 行内命中：用真实行位置（修复叠画后行 y 有效）
    QTextLine hitLine;
    for (const auto& l : b.lines)
        if (docPt.y() - b.y < l.y() + l.height() + lineGap_) { hitLine = l; break; }
    if (!hitLine.isValid() && !b.lines.isEmpty()) hitLine = b.lines.last();
    int charPos = hitLine.isValid()
                      ? hitLine.xToCursor(docPt.x() - b.textX, QTextLine::CursorOnCharacter)
                      : 0;
    // charPos（QString 字符下标）→ (run, off) UTF-8 字节偏移
    const oc::Paragraph* p = ed_->paragraphAt((size_t)b.flat);
    pos.para = (size_t)b.flat;
    if (!p || p->runs.empty()) { pos.run = 0; pos.off = 0; return pos; }
    int charsLeft = charPos;
    for (size_t r = 0; r < p->runs.size(); ++r) {
        QString rt = QString::fromStdString(p->runs[r].text);
        if (charsLeft <= rt.size()) {
            pos.run = r;
            // 字符数 → UTF-8 字节
            QByteArray u8 = rt.left(charsLeft).toUtf8();
            pos.off = (size_t)u8.size();
            return pos;
        }
        charsLeft -= rt.size();
    }
    pos.run = p->runs.size() - 1;
    pos.off = p->runs.back().text.size();
    return pos;
}

qreal PageView::cursorY() const {
    if (!ed_) return topPad_;
    // 找光标段落框
    for (const auto& b : boxes_)
        if ((size_t)b.flat == cursor_.para) return b.y;
    return topPad_;
}

QString PageView::paraText(size_t flat) const {
    const oc::Paragraph* p = ed_ ? ed_->paragraphAt(flat) : nullptr;
    if (!p) return QString();
    QString s;
    for (const auto& r : p->runs) s += QString::fromStdString(r.text);
    return s;
}

DocPos PageView::endOfPara(size_t flat) const {
    DocPos pos;
    pos.para = flat;
    const oc::Paragraph* p = ed_ ? ed_->paragraphAt(flat) : nullptr;
    if (!p || p->runs.empty()) { pos.run = 0; pos.off = 0; return pos; }
    pos.run = p->runs.size() - 1;
    pos.off = p->runs.back().text.size();
    return pos;
}

// ---------------- 绘制 ----------------

void PageView::paintEvent(QPaintEvent*) {
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), QColor(0xFF, 0xFF, 0xFF));

    if (!doc_ || !ed_) {
        p.setPen(QColor(0x90, 0x90, 0x90));
        p.drawText(viewport()->rect(), Qt::AlignCenter,
                   tr("Ctrl+O 打开 .docx\n打开后直接点击正文即可编辑"));
        return;
    }

    const int scrollY = verticalScrollBar()->value();
    const int W = viewport()->width();

    // ---- 表格（先画底：网格 + 表头底色，文本由段落盒层叠其上）----
    for (const auto& tb : tables_) {
        if (tb.y + tb.h < scrollY - 100 || tb.y > scrollY + viewport()->height() + 100)
            continue;
        QRectF rect(textMarginX_, tb.y - scrollY, tb.w, tb.h);
        // 表头底色（首行）
        p.fillRect(rect.adjusted(0, 0, 0, -(tb.h - 24 > 0 ? tb.h - 24 : 0)),
                   QColor(0xEA, 0xF1, 0xF9));
        // 网格
        p.setPen(QPen(QColor(0xB8, 0xC4, 0xD4), 1));
        p.drawRect(rect);
        for (size_t c = 0; c + 1 < tb.colX.size(); ++c) {
            qreal x = tb.colX[c] + tb.colW[c];
            p.drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        }
    }

    for (const auto& b : boxes_) {
        if (b.y + b.h < scrollY - 100 || b.y > scrollY + viewport()->height() + 100)
            continue;                                    // 视口裁剪
        const oc::Paragraph* para = ed_->paragraphAt((size_t)b.flat);
        if (!para) continue;

        // 选区高亮（段落文本范围）
        DocPos selStart = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos selEnd = cursor_ < anchor_ ? anchor_ : cursor_;
        // 简化选区渲染：仅当本段在选区内画整段底色（细化到字符 M3）
        bool paraInSel = (selStart.para <= (size_t)b.flat && (size_t)b.flat <= selEnd.para)
                         && !(anchor_ == cursor_);
        if (paraInSel) {
            p.fillRect(QRectF(textMarginX_, b.y - scrollY, W - 2 * textMarginX_, b.h).toRect(),
                       QColor(0xD8, 0xE4, 0xFA));
        }

        // 图片真实显示（ImageCache 按需解码 + LRU；未命中画灰框占位）
        if (!b.imgs.isEmpty()) {
            qreal ix = b.x, iy = b.y - scrollY;
            for (const auto& im : b.imgs) {
                const oc::DecodedImage* di = cache_
                    ? cache_->get(im.relId.toStdString(), (uint32_t)im.w, (uint32_t)im.h)
                    : nullptr;
                if (di && !di->rgba.empty()) {
                    QImage qimg(di->rgba.data(), (int)di->width, (int)di->height,
                                (int)di->width * 4, QImage::Format_RGBA8888);
                    p.drawImage(QRectF(ix, iy, im.w, im.h), qimg);
                } else {
                    p.fillRect(QRectF(ix, iy, im.w, im.h), QColor(0xF2, 0xF3, 0xF7));
                    p.setPen(QColor(0xC0, 0xC4, 0xCC));
                    p.drawRect(QRectF(ix, iy, im.w, im.h));
                }
                iy += im.h + 4;
            }
        }
        // 查找命中高亮（字符级黄底，同行区间）
        for (const auto& m : matches_) {
            if (m.flat != (size_t)b.flat) continue;
            QTextLine ls = b.layout->lineForTextPosition(m.start);
            QTextLine le = b.layout->lineForTextPosition(m.start + m.len);
            if (ls.isValid() && le.isValid() && ls.lineNumber() == le.lineNumber()) {
                qreal x1 = ls.cursorToX(m.start), x2 = le.cursorToX(m.start + m.len);
                p.fillRect(QRectF(b.textX + x1, b.y - scrollY + ls.y(),
                                  x2 - x1, ls.height()).toRect(),
                           QColor(255, 235, 59, 160));
            }
        }
        // 文本（含每 run 富格式）
        b.layout->draw(&p, QPointF(b.textX, b.y - scrollY));

        // 图片占位（段内含图）
        if (!para->images.empty()) {
            p.setPen(QColor(0x08, 0x6A, 0xD2));
            p.drawRect(QRect(textMarginX_, int(b.y - scrollY + b.h - paraGap_ - 18), 88, 16));
            p.drawText(QRect(textMarginX_, int(b.y - scrollY + b.h - paraGap_ - 18), 88, 16),
                       Qt::AlignCenter, QStringLiteral("图片×%1").arg(para->images.size()));
        }
    }

    // 光标（闪烁；字符级行定位——真机战报 #2 修复：光标在光标所在行，不再固定首行）
    if (cursorVisible_ && hasFocus()) {
        for (const auto& b : boxes_) {
            if ((size_t)b.flat != cursor_.para) continue;
            const oc::Paragraph* para = ed_->paragraphAt(cursor_.para);
            int charPos = 0;
            if (para) {
                for (size_t r = 0; r < cursor_.run && r < para->runs.size(); ++r)
                    charPos += QString::fromStdString(para->runs[r].text).size();
                charPos += QString::fromStdString(
                               para->runs[cursor_.run].text).left(int(cursor_.off)).size();
            }
            QTextLine line = b.layout->lineForTextPosition(charPos);
            if (!line.isValid() && !b.lines.isEmpty()) line = b.lines.last();
            if (line.isValid()) {
                qreal cx = b.textX + line.cursorToX(charPos);
                qreal cy = b.y - scrollY + line.y();
                p.setPen(QColor(0x20, 0x20, 0x20));
                p.drawLine(QPointF(cx, cy + 1), QPointF(cx, cy + line.height() + 1));
            }
            break;
        }
    }
}

void PageView::timerEvent(QTimerEvent* e) {
    if (e->timerId() == blinkTimer_) {
        cursorVisible_ = !cursorVisible_;
        viewport()->update();
    }
    QAbstractScrollArea::timerEvent(e);
}

// ---------------- 交互 ----------------

void PageView::startFormatBrush() {
    if (!ed_) return;
    const oc::Paragraph* p = ed_->paragraphAt(cursor_.para);
    if (!p || p->runs.empty()) return;
    brushFmt_ = p->runs[std::min(cursor_.run, p->runs.size() - 1)];
    brushActive_ = true;
    viewport()->setCursor(Qt::CrossCursor);
}

void PageView::highlightAllMatches(const QString& needle) {
    matches_.clear();
    if (!ed_ || needle.isEmpty()) { viewport()->update(); return; }
    const size_t n = ed_->paragraphCount();
    for (size_t flat = 0; flat < n; ++flat) {
        QString text = paraText(flat);
        int at = 0;
        while ((at = text.indexOf(needle, at, Qt::CaseInsensitive)) >= 0) {
            matches_.push_back({flat, at, (int)needle.size()});
            at += needle.size();
        }
    }
    viewport()->update();
}

void PageView::mousePressEvent(QMouseEvent* e) {
    if (!ed_) return;
    setFocus();
    QPointF docPt(e->pos().x() + 0, e->pos().y() + verticalScrollBar()->value());
    DocPos pos = hitPos(docPt);
    if (e->modifiers() & Qt::ShiftModifier) {
        anchor_ = anchor_;                              // Shift+点击扩选
    } else {
        anchor_ = pos;
    }
    cursor_ = pos;
    // 格式刷：应用缓存的格式到目标段（或选区）
    if (brushActive_ && ed_) {
        brushActive_ = false;
        viewport()->setCursor(Qt::IBeamCursor);
        const bool hasSel = !(anchor_ == cursor_);
        DocPos a = hasSel ? (anchor_ < cursor_ ? anchor_ : cursor_) : DocPos{pos.para, 0, 0};
        DocPos b = hasSel ? (cursor_ < anchor_ ? anchor_ : cursor_)
                          : endOfPara(pos.para);
        auto& F = brushFmt_;
        if (F.bold || !F.bold) ed_->setBold(a, b, F.bold);
        ed_->setItalic(a, b, F.italic);
        ed_->setUnderline(a, b, F.underline);
        ed_->setStrike(a, b, F.strike);
        if (F.fontSizePt > 0) ed_->setFontSize(a, b, F.fontSizePt);
        if (!F.fontEastAsia.empty()) ed_->setFontName(a, b, F.fontEastAsia, F.fontAscii);
        if (!F.color.empty()) ed_->setTextColor(a, b, F.color);
        if (!F.highlight.empty()) ed_->setHighlight(a, b, F.highlight);
        relayoutAndKeepCursor();
        emit edited();
        return;
    }
    mouseDragging_ = true;
    viewport()->update();
    emit cursorMoved();
}

void PageView::mouseMoveEvent(QMouseEvent* e) {
    if (!mouseDragging_ || !ed_) return;
    QPointF docPt(e->pos().x(), e->pos().y() + verticalScrollBar()->value());
    cursor_ = hitPos(docPt);
    viewport()->update();
    emit cursorMoved();
}

void PageView::mouseReleaseEvent(QMouseEvent*) {
    mouseDragging_ = false;
}

void PageView::keyPressEvent(QKeyEvent* e) {
    if (!ed_ || !cs_) { QAbstractScrollArea::keyPressEvent(e); return; }
    const bool hasSel = !(anchor_ == cursor_);
    const bool keepAnchor = (e->modifiers() & Qt::ShiftModifier) != 0;   // Shift+方向键扩选
    DocPos selStart = anchor_ < cursor_ ? anchor_ : cursor_;
    DocPos selEnd = cursor_ < anchor_ ? anchor_ : cursor_;

    // 快捷键
    if (e->modifiers() & Qt::ControlModifier) {
        switch (e->key()) {
        case Qt::Key_Z: undo(); return;
        case Qt::Key_Y: redo(); return;
        case Qt::Key_B: boldSelection(!selectionBoldHint()); return;
        default: QAbstractScrollArea::keyPressEvent(e); return;
        }
    }

    switch (e->key()) {
    case Qt::Key_Backspace:
        if (hasSel) ed_->deleteRange(selStart, selEnd);
        else {
            // 前退一字（UTF-8 字符边界）
            DocPos prev = cursor_;
            if (cursor_.off > 0) {
                const oc::Paragraph* p = ed_->paragraphAt(cursor_.para);
                const std::string& t = p->runs[cursor_.run].text;
                size_t off = cursor_.off - 1;
                while (off > 0 && (t[off] & 0xC0) == 0x80) --off;  // 回退到字符头
                prev.off = off;
            } else if (cursor_.run > 0) {
                prev.run = cursor_.run - 1;
                prev.off = ed_->paragraphAt(cursor_.para)->runs[prev.run].text.size();
            }
            if (!(prev == cursor_)) { ed_->deleteRange(prev, cursor_); cursor_ = prev; }
        }
        break;
    case Qt::Key_Delete:
        if (hasSel) ed_->deleteRange(selStart, selEnd);
        else {
            DocPos next = cursor_;
            const oc::Paragraph* p = ed_->paragraphAt(cursor_.para);
            const std::string& t = p->runs[cursor_.run].text;
            if (cursor_.off < t.size()) {
                size_t off = cursor_.off + 1;
                while (off < t.size() && (t[off] & 0xC0) == 0x80) ++off;
                next.off = off;
                ed_->deleteRange(cursor_, next);
            }
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (hasSel) { ed_->deleteRange(selStart, selEnd); cursor_ = selStart; }
        // 列表自动续号：前段是 •/N. 前缀 → 新段继承（编号+1；空列表项回车=退出列表）
        QString curText = paraText(cursor_.para);
        QString carry;
        if (curText.startsWith(QStringLiteral("• "))) carry = QStringLiteral("• ");
        else {
            QRegularExpression re(QStringLiteral("^(\\d+)\\. "));
            auto m = re.match(curText);
            if (m.hasMatch()) carry = QString::number(m.captured(1).toInt() + 1) + QStringLiteral(". ");
        }
        if (!carry.isEmpty() && curText.trimmed() == carry.trimmed()) carry.clear();  // 空项退出
        ed_->splitParagraph(cursor_);
        ++cursor_.para; cursor_.run = 0; cursor_.off = 0;
        if (!carry.isEmpty()) {
            ed_->insertText(cursor_, carry.toStdString());
            cursor_.off += carry.toUtf8().size();
        }
        break;
    }
    case Qt::Key_Left: {
        if (hasSel && !keepAnchor) { cursor_ = selStart; }
        else if (cursor_.off > 0) {
            const std::string& t = ed_->paragraphAt(cursor_.para)->runs[cursor_.run].text;
            size_t off = cursor_.off - 1;
            while (off > 0 && (t[off] & 0xC0) == 0x80) --off;
            cursor_.off = off;
        } else if (cursor_.run > 0) {
            --cursor_.run;
            cursor_.off = ed_->paragraphAt(cursor_.para)->runs[cursor_.run].text.size();
        } else if (cursor_.para > 0) {
            --cursor_.para;
            cursor_ = endOfPara(cursor_.para);
        }
        if (!keepAnchor) anchor_ = cursor_;
        break;
    }
    case Qt::Key_Right: {
        if (hasSel && !keepAnchor) { cursor_ = selEnd; }
        else {
            const oc::Paragraph* p = ed_->paragraphAt(cursor_.para);
            const std::string& t = p->runs[cursor_.run].text;
            if (cursor_.off < t.size()) {
                size_t off = cursor_.off + 1;
                while (off < t.size() && (t[off] & 0xC0) == 0x80) ++off;
                cursor_.off = off;
            } else if (cursor_.run + 1 < p->runs.size()) {
                ++cursor_.run; cursor_.off = 0;
            } else if (cursor_.para + 1 < ed_->paragraphCount()) {
                ++cursor_.para; cursor_.run = 0; cursor_.off = 0;
            }
        }
        if (!keepAnchor) anchor_ = cursor_;
        break;
    }
    case Qt::Key_Up:
        if (cursor_.para > 0) { --cursor_.para; cursor_ = endOfPara(cursor_.para); }
        if (!keepAnchor) anchor_ = cursor_;
        break;
    case Qt::Key_Down:
        if (cursor_.para + 1 < ed_->paragraphCount()) {
            ++cursor_.para; cursor_.run = 0; cursor_.off = 0;
        }
        if (!keepAnchor) anchor_ = cursor_;
        break;
    case Qt::Key_Home: cursor_.run = 0; cursor_.off = 0; if (!keepAnchor) anchor_ = cursor_; break;
    case Qt::Key_End: cursor_ = endOfPara(cursor_.para); if (!keepAnchor) anchor_ = cursor_; break;
    default:
        // 普通文本输入（ASCII/直接键入；中文走 inputMethodEvent）
        if (!e->text().isEmpty()) {
            QString s = e->text();
            if (s.at(0).isPrint()) {
                if (hasSel) { ed_->deleteRange(selStart, selEnd); cursor_ = selStart; }
                ed_->insertText(cursor_, s.toUtf8().toStdString());
                cursor_.off += s.toUtf8().size();
            }
        }
        break;
    }
    anchor_ = anchor_ == cursor_ ? cursor_ : anchor_;
    relayoutAndKeepCursor();
    emit cursorMoved();
    // 纯移动键不标 dirty（Backspace/Delete/Return/输入已改文档；移动键无编辑）
    if (e->key() != Qt::Key_Left && e->key() != Qt::Key_Right &&
        e->key() != Qt::Key_Up && e->key() != Qt::Key_Down &&
        e->key() != Qt::Key_Home && e->key() != Qt::Key_End)
        emit edited();
}

void PageView::inputMethodEvent(QInputMethodEvent* e) {
    if (!ed_ || !e->commitString().isEmpty()) {
        const QString& s = e->commitString();
        if (!s.isEmpty() && ed_) {
            const bool hasSel = !(anchor_ == cursor_);
            DocPos selStart = anchor_ < cursor_ ? anchor_ : cursor_;
            DocPos selEnd = cursor_ < anchor_ ? anchor_ : cursor_;
            if (hasSel) { ed_->deleteRange(selStart, selEnd); cursor_ = selStart; }
            ed_->insertText(cursor_, s.toUtf8().toStdString());
            cursor_.off += s.toUtf8().size();
            anchor_ = cursor_;
            relayoutAndKeepCursor();
            emit edited();
        }
    }
    viewport()->update();
}

QVariant PageView::inputMethodQuery(Qt::InputMethodQuery q) const {
    if (q == Qt::ImEnabled) return true;
    if (q == Qt::ImCursorRectangle) {
        // 光标矩形（输入法候选框定位）
        for (const auto& b : boxes_)
            if ((size_t)b.flat == cursor_.para) {
                qreal y = b.y - verticalScrollBar()->value();
                return QRect(int(textMarginX_), int(y), 1,
                             int(b.lines.isEmpty() ? 18 : b.lines[0].height()));
            }
    }
    return QVariant();
}

void PageView::relayoutAndKeepCursor() {
    matches_.clear();          // 内容变化后命中高亮失效
    rebuildLayout();
    ensureVisible(cursorY());
    cursorVisible_ = true;
    viewport()->update();
}

void PageView::ensureVisible(qreal docY) {
    int y = (int)docY;
    if (y < verticalScrollBar()->value())
        verticalScrollBar()->setValue(y);
    else if (y + 20 > verticalScrollBar()->value() + viewport()->height())
        verticalScrollBar()->setValue(y + 20 - viewport()->height());
}

// ---------------- 编辑 API ----------------

bool PageView::canUndo() const { return cs_ && cs_->canUndo(); }
bool PageView::canRedo() const { return cs_ && cs_->canRedo(); }

bool PageView::undo() {
    if (!cs_ || !cs_->undo()) return false;
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::redo() {
    if (!cs_ || !cs_->redo()) return false;
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::selectionBoldHint() const {
    if (!ed_) return false;
    const oc::Paragraph* p = ed_->paragraphAt(cursor_.para);
    return p && !p->runs.empty() && p->runs[std::min(cursor_.run, p->runs.size() - 1)].bold;
}

bool PageView::boldSelection(bool val) {
    if (!ed_) return false;
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setBold(a, b, val);
    } else {
        ed_->setBold(cursor_, cursor_, val);
    }
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::italicSelection(bool val) {
    if (!ed_) return false;
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setItalic(a, b, val);
    } else ed_->setItalic(cursor_, cursor_, val);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::underlineSelection(bool val) {
    if (!ed_) return false;
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setUnderline(a, b, val);
    } else ed_->setUnderline(cursor_, cursor_, val);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::fontSizeSelection(double pt) {
    if (!ed_ || pt < 4 || pt > 200) return false;
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setFontSize(a, b, pt);
    } else ed_->setFontSize(cursor_, cursor_, pt);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::fontNameSelection(const QString& family) {
    if (!ed_) return false;
    const std::string n = family.toStdString();
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setFontName(a, b, n, n);
    } else ed_->setFontName(cursor_, cursor_, n, n);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::strikeSelection(bool val) {
    if (!ed_) return false;
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setStrike(a, b, val);
    } else ed_->setStrike(cursor_, cursor_, val);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::textColorSelection(const QString& hexRgb) {
    if (!ed_) return false;
    const std::string n = hexRgb.toStdString();
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setTextColor(a, b, n);
    } else ed_->setTextColor(cursor_, cursor_, n);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::highlightSelection(const QString& colorName) {
    if (!ed_) return false;
    const std::string n = colorName.toStdString();
    const bool hasSel = !(anchor_ == cursor_);
    if (hasSel) {
        DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
        DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
        ed_->setHighlight(a, b, n);
    } else ed_->setHighlight(cursor_, cursor_, n);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::lineSpacingSelection(double factor) {
    if (!ed_) return false;
    return ed_->setLineSpacing(cursor_.para, (uint32_t)(factor * 240 + 0.5)) &&
           (relayoutAndKeepCursor(), emit edited(), true);
}

bool PageView::indentSelection(int deltaChars) {
    if (!ed_) return false;
    const Paragraph* p = ed_->paragraphAt(cursor_.para);
    if (!p) return false;
    int left = (int)p->indLeftTw + deltaChars * 240;
    if (left < 0) left = 0;
    bool ok = ed_->setIndent(cursor_.para, (uint32_t)left, p->indRightTw, p->indFirstLineTw);
    if (ok) { relayoutAndKeepCursor(); emit edited(); }
    return ok;
}

bool PageView::firstLineIndentSelection() {
    if (!ed_) return false;
    const Paragraph* p = ed_->paragraphAt(cursor_.para);
    if (!p) return false;
    int fl = p->indFirstLineTw != 0 ? 0 : 480;         // 切换：2字符/无
    bool ok = ed_->setIndent(cursor_.para, p->indLeftTw, p->indRightTw, fl);
    if (ok) { relayoutAndKeepCursor(); emit edited(); }
    return ok;
}

bool PageView::toggleList(bool numbered) {
    if (!ed_) return false;
    QString text = paraText(cursor_.para);
    QString prefix = numbered ? numberedPrefixOf(cursor_.para) : QStringLiteral("• ");
    QString stripped = text;
    for (const QString& pre : {QStringLiteral("• "), QStringLiteral("1. "), QStringLiteral("2. "),
                                QStringLiteral("3. "), QStringLiteral("4. "), QStringLiteral("5. ")}) {
        if (stripped.startsWith(pre)) { stripped.remove(0, pre.size()); break; }
    }
    QString newText = (stripped == text) ? prefix + text : stripped;
    const Paragraph* p = ed_->paragraphAt(cursor_.para);
    RunList newRuns;
    if (p && !p->runs.empty()) {
        TextRun r = p->runs.front();
        r.text = newText.toStdString();
        newRuns.push_back(r);
    } else {
        TextRun r; r.text = newText.toStdString();
        newRuns.push_back(r);
    }
    RunList oldRuns = p ? p->runs : RunList{};
    ed_->replaceRunsCommand(cursor_.para, std::move(oldRuns), std::move(newRuns), "list");
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

QString PageView::numberedPrefixOf(size_t flat) const {
    // 向上数连续编号段，序号+1
    int n = 1;
    for (size_t i = flat; i > 0; --i) {
        QString t = paraText(i - 1);
        QRegularExpression re(QStringLiteral("^\\d+\\. "));
        auto m = re.match(t);
        if (m.hasMatch()) ++n;
        else break;
    }
    return QString::number(n) + QStringLiteral(". ");
}

bool PageView::paraStyleSelection(const QString& styleId) {
    if (!ed_) return false;
    ed_->setParaStyle(cursor_.para, styleId.toStdString());
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

bool PageView::alignSelection(int align) {
    if (!ed_) return false;
    oc::Align a = align == 1 ? oc::Align::Center
               : align == 2 ? oc::Align::Right
               : align == 3 ? oc::Align::Justify
               : oc::Align::Left;
    ed_->setAlign(cursor_.para, a);
    relayoutAndKeepCursor();
    emit edited();
    return true;
}

PageView::FormatHints PageView::formatHints() const {
    FormatHints h;
    if (!ed_) return h;
    h.hasDoc = true;
    const oc::Paragraph* p = ed_->paragraphAt(cursor_.para);
    if (!p) return h;
    h.styleId = QString::fromStdString(p->styleId);
    h.align = (int)p->align;
    if (!p->runs.empty()) {
        const auto& r = p->runs[std::min(cursor_.run, p->runs.size() - 1)];
        h.bold = r.bold;
        h.italic = r.italic;
        h.underline = r.underline;
        h.strike = r.strike;
        h.sizePt = r.fontSizePt;
        h.eastAsia = QString::fromStdString(r.fontEastAsia);
        h.color = QString::fromStdString(r.color);
        h.highlight = QString::fromStdString(r.highlight);
    }
    return h;
}

void PageView::copySelection() {
    if (!ed_ || anchor_ == cursor_) return;
    DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
    DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
    auto frag = ed_->copyRange(a, b);
    QGuiApplication::clipboard()->setText(QString::fromStdString(frag.plainText()));
}

void PageView::cutSelection() {
    copySelection();
    if (!ed_ || anchor_ == cursor_) return;
    DocPos a = anchor_ < cursor_ ? anchor_ : cursor_;
    DocPos b = cursor_ < anchor_ ? anchor_ : cursor_;
    ed_->deleteRange(a, b);
    cursor_ = a; anchor_ = a;
    relayoutAndKeepCursor();
    emit edited();
}

void PageView::pasteClipboard() {
    if (!ed_) return;
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) return;
    const bool hasSel = !(anchor_ == cursor_);
    DocPos selStart = anchor_ < cursor_ ? anchor_ : cursor_;
    DocPos selEnd = cursor_ < anchor_ ? anchor_ : cursor_;
    if (hasSel) { ed_->deleteRange(selStart, selEnd); cursor_ = selStart; }
    auto frag = oc::ClipboardFragment::fromPlainText(text.toStdString());
    if (frag.paras.size() == 1) {
        ed_->insertText(cursor_, text.toUtf8().toStdString());
        cursor_.off += text.toUtf8().size();
    } else {
        ed_->paste(cursor_, frag);
        cursor_.para += frag.paras.size() - 1;
        cursor_ = endOfPara(cursor_.para);
    }
    anchor_ = cursor_;
    relayoutAndKeepCursor();
    emit edited();
}

void PageView::selectAll() {
    if (!ed_ || ed_->paragraphCount() == 0) return;
    anchor_ = DocPos{};
    cursor_ = endOfPara(ed_->paragraphCount() - 1);
    viewport()->update();
}

DocPos PageView::charToDocPos(size_t flat, int charIdx) const {
    DocPos pos;
    pos.para = flat;
    const oc::Paragraph* p = ed_ ? ed_->paragraphAt(flat) : nullptr;
    if (!p || p->runs.empty()) return pos;
    int left = charIdx;
    for (size_t r = 0; r < p->runs.size(); ++r) {
        int chars = (int)QString::fromStdString(p->runs[r].text).size();
        if (left <= chars) {
            pos.run = r;
            pos.off = (size_t)QString::fromStdString(p->runs[r].text)
                          .left(left).toUtf8().size();
            return pos;
        }
        left -= chars;
    }
    pos.run = p->runs.size() - 1;
    pos.off = p->runs.back().text.size();
    return pos;
}

bool PageView::findNext(const QString& needle) {
    if (!ed_ || needle.isEmpty()) return false;
    const size_t n = ed_->paragraphCount();
    // 从光标处顺向环形搜索
    int startChar = 0;
    {
        const oc::Paragraph* cp = ed_->paragraphAt(cursor_.para);
        if (cp) {
            int c = 0;
            for (size_t r = 0; r < cursor_.run && r < cp->runs.size(); ++r)
                c += QString::fromStdString(cp->runs[r].text).size();
            c += (int)QString::fromStdString(
                     cp->runs[cursor_.run].text).left((int)cursor_.off).size();
            startChar = c + 1;                      // 光标之后
        }
    }
    for (size_t off = 0; off < n; ++off) {
        size_t flat = cursor_.para + off < n ? cursor_.para + off : cursor_.para + off - n;
        QString text = paraText(flat);
        int from = (off == 0) ? startChar : 0;
        int idx = text.indexOf(needle, from, Qt::CaseInsensitive);
        if (idx >= 0) {
            DocPos a = charToDocPos(flat, idx);
            DocPos b = charToDocPos(flat, idx + needle.size());
            anchor_ = a;
            cursor_ = b;
            scrollToPara(flat);
            return true;
        }
    }
    return false;
}

int PageView::replaceAll(const QString& from, const QString& to) {
    if (!ed_ || from.isEmpty()) return 0;
    int count = 0;
    const size_t n = ed_->paragraphCount();
    for (size_t flat = 0; flat < n; ++flat) {
        const oc::Paragraph* p = ed_->paragraphAt(flat);
        if (!p) continue;
        QString text = paraText(flat);
        int hits = 0, at = 0;
        while ((at = text.indexOf(from, at, Qt::CaseInsensitive)) >= 0) {
            ++hits; at += from.size();
        }
        if (!hits) continue;
        QString replaced = text;
        replaced.replace(from, to, Qt::CaseInsensitive);
        // 保留段首 run 格式的新文本（段落级快照命令，可整体撤销）
        oc::RunList newRuns;
        oc::TextRun r = p->runs.empty() ? oc::TextRun{} : p->runs.front();
        r.text = replaced.toStdString();
        newRuns.push_back(r);
        oc::RunList oldRuns = p->runs;
        ed_->replaceRunsCommand(flat, std::move(oldRuns), std::move(newRuns),
                                QStringLiteral("replace").toStdString());
        count += hits;
    }
    if (count) {
        relayoutAndKeepCursor();
        emit edited();
    }
    return count;
}

void PageView::pasteTextAtCursor(const QString& text) {
    if (!ed_ || text.isEmpty()) return;
    ed_->insertText(cursor_, text.toUtf8().toStdString());
    cursor_.off += text.toUtf8().size();
    anchor_ = cursor_;
    relayoutAndKeepCursor();
    emit edited();
}

void PageView::pasteText(const QString& text) {
    if (!ed_ || text.isEmpty()) return;
    const bool hasSel = !(anchor_ == cursor_);
    DocPos selStart = anchor_ < cursor_ ? anchor_ : cursor_;
    DocPos selEnd = cursor_ < anchor_ ? anchor_ : cursor_;
    if (hasSel) { ed_->deleteRange(selStart, selEnd); cursor_ = selStart; }
    ed_->insertText(cursor_, text.toUtf8().toStdString());
    cursor_.off += text.toUtf8().size();
    anchor_ = cursor_;
    relayoutAndKeepCursor();
    emit edited();
}

void PageView::scrollToPara(size_t flat) {
    for (const auto& b : boxes_)
        if ((size_t)b.flat == flat) {
            verticalScrollBar()->setValue((int)b.y - 60);
            cursor_ = DocPos{flat, 0, 0};
            anchor_ = cursor_;
            viewport()->update();
            emit cursorMoved();
            return;
        }
}

void PageView::moveCursorToStart() {
    cursor_ = DocPos{};
    anchor_ = cursor_;
    relayoutAndKeepCursor();
}

} // namespace oc::qt
