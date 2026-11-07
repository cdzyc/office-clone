#include "PageView.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextOption>
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

// ---------------- 布局 ----------------

void PageView::rebuildLayout() {
    boxes_.clear();
    docHeight_ = topPad_;
    if (!doc_) {
        updateScrollRange();
        return;
    }
    // 编辑视图：全量段落布局（编辑后调用；2 万段实测毫秒级，动态视口裁剪只作用于绘制）
    const size_t n = ed_ ? ed_->paragraphCount() : 0;
    for (size_t i = 0; i < n; ++i) {
        const oc::Paragraph* p = ed_ ? ed_->paragraphAt(i) : nullptr;
        if (!p) break;
        ParaBox b;
        b.flat = (int)i;
        b.font = fontFor(*p, font());
        QString text;
        for (const auto& r : p->runs) text += QString::fromStdString(r.text);
        if (!p->images.empty()) text += QStringLiteral(" ▣图片%1 ").arg(p->images.size());

        b.layout = std::make_unique<QTextLayout>(text, b.font);
        b.layout->setTextOption(QTextOption(alignFor(*p)));
        b.layout->beginLayout();
        forever {
            QTextLine line = b.layout->createLine();
            if (!line.isValid()) break;
            line.setLineWidth(qreal(std::max(120, viewport()->width() - 2 * textMarginX_)));
            line.setPosition(QPointF(0, 0));
            b.lines.append(line);
        }
        b.layout->endLayout();
        // 段高 = 行高 + 行距
        qreal textW = 0;
        for (const auto& l : b.lines) textW = std::max(textW, l.height());
        b.h = b.lines.size() * (b.layout->lineCount() ? b.lines[0].height() : 18)
              + (b.lines.size() ? lineGap_ * (b.lines.size()) : 0)
              + paraGap_;
        b.y = docHeight_;
        docHeight_ += b.h;
        boxes_.push_back(std::move(b));
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
    // 行内命中
    int lineIdx = 0;
    for (int li = 0; li < b.lines.size(); ++li) {
        qreal lh = b.lines[(qsizetype)li].height() + lineGap_;
        if (docPt.y() < b.y + (li + 1) * lh) { lineIdx = li; break; }
        lineIdx = li;
    }
    QTextLine line = b.lines[lineIdx];
    int charPos = line.xToCursor(docPt.x() - textMarginX_, QTextLine::CursorOnCharacter);
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

    for (const auto& b : boxes_) {
        if (b.y + b.h < scrollY - 100 || b.y > scrollY + viewport()->height() + 100)
            continue;                                    // 视口裁剪
        const oc::Paragraph* para = ed_->paragraphAt((size_t)b.flat);
        if (!para) continue;

        // 选区高亮（段落文本范围）
        bool selStart = anchor_ < cursor_ ? anchor_ : cursor_;
        bool selEnd = cursor_ < anchor_ ? anchor_ : cursor_;
        // 简化选区渲染：仅当本段在选区内画整段底色（细化到字符 M3）
        bool paraInSel = (selStart.para <= (size_t)b.flat && (size_t)b.flat <= selEnd.para)
                         && !(anchor_ == cursor_);
        if (paraInSel) {
            p.fillRect(QRectF(textMarginX_, b.y - scrollY, W - 2 * textMarginX_, b.h).toRect(),
                       QColor(0xD8, 0xE4, 0xFA));
        }

        // 文本（含每 run 富格式：逐 run 分段绘制会破坏整段断行——首版整段用统一字体，
        // 富格式（加粗等）视觉从 M2.5+ 的 QTextLayout setFormats 接入）
        b.layout->draw(&p, QPointF(textMarginX_, b.y - scrollY));

        // 图片占位（段内含图）
        if (!para->images.empty()) {
            p.setPen(QColor(0x08, 0x6A, 0xD2));
            p.drawRect(QRect(textMarginX_, int(b.y - scrollY + b.h - paraGap_ - 18), 88, 16));
            p.drawText(QRect(textMarginX_, int(b.y - scrollY + b.h - paraGap_ - 18), 88, 16),
                       Qt::AlignCenter, QStringLiteral("图片×%1").arg(para->images.size()));
        }
    }

    // 光标（闪烁；选区拖拽时也显示）
    if (cursorVisible_ && hasFocus()) {
        for (const auto& b : boxes_) {
            if ((size_t)b.flat != cursor_.para) continue;
            // 光标行/位置（字符级 M3：当前画在段首行尾近似）
            qreal y = b.y - scrollY;
            qreal lh = b.lines.isEmpty() ? 18 : b.lines[0].height();
            // 光标 x：run 内偏移 → 整段字符位 → 行内 x
            int charPos = 0;
            const oc::Paragraph* para = ed_->paragraphAt(cursor_.para);
            if (para) {
                for (size_t r = 0; r < cursor_.run && r < para->runs.size(); ++r)
                    charPos += QString::fromStdString(para->runs[r].text).size();
                charPos += QString::fromStdString(
                               para->runs[cursor_.run].text).left(int(cursor_.off)).size();
            }
            qreal cx = textMarginX_;
            if (!b.lines.isEmpty())
                cx += b.lines[0].cursorToX(charPos);
            p.setPen(QColor(0x20, 0x20, 0x20));
            p.drawLine(QPointF(cx, y + 1), QPointF(cx, y + lh + 1));
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
    mouseDragging_ = true;
    viewport()->update();
}

void PageView::mouseMoveEvent(QMouseEvent* e) {
    if (!mouseDragging_ || !ed_) return;
    QPointF docPt(e->pos().x(), e->pos().y() + verticalScrollBar()->value());
    cursor_ = hitPos(docPt);
    viewport()->update();
}

void PageView::mouseReleaseEvent(QMouseEvent*) {
    mouseDragging_ = false;
}

void PageView::keyPressEvent(QKeyEvent* e) {
    if (!ed_ || !cs_) { QAbstractScrollArea::keyPressEvent(e); return; }
    const bool hasSel = !(anchor_ == cursor_);
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
    case Qt::Key_Enter:
        if (hasSel) { ed_->deleteRange(selStart, selEnd); cursor_ = selStart; }
        ed_->splitParagraph(cursor_);
        ++cursor_.para; cursor_.run = 0; cursor_.off = 0;
        break;
    case Qt::Key_Left: {
        if (hasSel) { cursor_ = selStart; anchor_ = cursor_; }
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
        anchor_ = cursor_;
        break;
    }
    case Qt::Key_Right: {
        if (hasSel) { cursor_ = selEnd; anchor_ = cursor_; }
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
        anchor_ = cursor_;
        break;
    }
    case Qt::Key_Up:
        if (cursor_.para > 0) { --cursor_.para; cursor_ = endOfPara(cursor_.para); }
        anchor_ = cursor_;
        break;
    case Qt::Key_Down:
        if (cursor_.para + 1 < ed_->paragraphCount()) {
            ++cursor_.para; cursor_.run = 0; cursor_.off = 0;
        }
        anchor_ = cursor_;
        break;
    case Qt::Key_Home: cursor_.run = 0; cursor_.off = 0; anchor_ = cursor_; break;
    case Qt::Key_End: cursor_ = endOfPara(cursor_.para); anchor_ = cursor_; break;
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

void PageView::moveCursorToStart() {
    cursor_ = DocPos{};
    anchor_ = cursor_;
    relayoutAndKeepCursor();
}

} // namespace oc::qt
