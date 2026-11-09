#include "PdfExporter.h"
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QPagedPaintDevice>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPrintDialog>
#include <QPrinter>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <cmath>

namespace oc::qt {
namespace {

qreal tw2px(uint32_t tw) { return tw / 15.0; }

QFont fontFor(const oc::Paragraph& para) {
    QFont f(QStringLiteral("Microsoft YaHei"));
    if (para.styleId == "Title")          { f.setPointSizeF(17); f.setBold(true); }
    else if (para.styleId == "Heading1")  { f.setPointSizeF(15); f.setBold(true); }
    else if (para.styleId == "Heading2")  { f.setPointSizeF(13); f.setBold(true); }
    else if (para.styleId == "Heading3")  { f.setPointSizeF(11.5); f.setBold(true); }
    else                                   f.setPointSizeF(10.5);
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

struct ParaLayout {
    std::unique_ptr<QTextLayout> layout;
    qreal h = 0;
    QFont font;
};

ParaLayout layoutPara(const oc::Paragraph& p, qreal width) {
    ParaLayout pl;
    pl.font = fontFor(p);
    QString text;
    for (const auto& r : p.runs) text += QString::fromStdString(r.text);
    if (!p.images.empty()) text += QStringLiteral(" ▣[图] ");

    // 富格式（B/I/U/S/字号/字体/颜色）
    QVector<QTextLayout::FormatRange> fmts;
    int pos = 0;
    for (const auto& r : p.runs) {
        int chars = (int)QString::fromStdString(r.text).size();
        if (!chars) { continue; }
        bool diff = false;
        QFont f = pl.font;
        QTextCharFormat fmt;
        if (r.bold)      { f.setBold(true); diff = true; }
        if (r.italic)    { f.setItalic(true); diff = true; }
        if (r.underline) { f.setUnderline(true); diff = true; }
        if (r.strike)    { f.setStrikeOut(true); diff = true; }
        if (r.fontSizePt > 0) { f.setPointSizeF(r.fontSizePt); diff = true; }
        if (!r.fontEastAsia.empty()) {
            f.setFamily(QString::fromStdString(r.fontEastAsia)); diff = true;
        }
        if (diff) {
            fmt.setFont(f);
            fmt.setFontFamilies(QStringList() << f.family());
            if (!r.color.empty())
                fmt.setForeground(QBrush(QColor(QString::fromStdString("#" + r.color))));
            QTextLayout::FormatRange fr;
            fr.start = pos; fr.length = chars; fr.format = fmt;
            fmts.append(fr);
        }
        pos += chars;
    }

    pl.layout = std::make_unique<QTextLayout>(text, pl.font);
    pl.layout->setTextOption(QTextOption(alignFor(p)));
    pl.layout->setFormats(fmts);
    pl.layout->beginLayout();
    qreal y = 0;
    forever {
        QTextLine line = pl.layout->createLine();
        if (!line.isValid()) break;
        line.setLineWidth(width);
        line.setPosition(QPointF(0, y));
        y += line.height() + 4;
    }
    pl.layout->endLayout();
    pl.h = qMax(y - 4, 18.0) + 10;
    return pl;
}

} // namespace

bool PdfExporter::exportPdf(const QString& pdfPath, const oc::Document& doc,
                            oc::ImageCache* cache, QString* err) {
    QPdfWriter writer(pdfPath);
    writer.setResolution(96);
    // 页面尺寸/边距按文档设置（twips→mm：1twip=1/1440in）
    QPageSize ps(QSizeF(doc.page.pageWidthTw / 1440.0, doc.page.pageHeightTw / 1440.0),
                 QPageSize::Inch);
    writer.setPageSize(ps);
    writer.setPageMargins(QMarginsF(doc.page.marginLeftTw / 1440.0,
                                    doc.page.marginTopTw / 1440.0,
                                    doc.page.marginRightTw / 1440.0,
                                    doc.page.marginBottomTw / 1440.0),
                          QPageLayout::Inch);
    bool ok = paintPages(&writer, doc, cache, err);
    if (ok) writer.setTitle(QFileInfo(pdfPath).completeBaseName());
    return ok;
}

bool PdfExporter::printDialog(QWidget* parent, const oc::Document& doc,
                              oc::ImageCache* cache, QString* err) {
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, parent);
    dlg.setWindowTitle(QObject::tr("打印"));
    if (dlg.exec() != QDialog::Accepted) return false;
    printer.setResolution(96);
    return paintPages(&printer, doc, cache, err);
}

bool PdfExporter::paintPages(QPagedPaintDevice* dev, const oc::Document& doc,
                             oc::ImageCache* cache, QString* err) {
    if (!dev) { if (err) *err = "null device"; return false; }
    QPainter painter(dev);
    if (!painter.isActive()) { if (err) *err = "painter inactive"; return false; }

    // 逻辑坐标：96dpi px（与屏幕布局一致）
    const qreal pageW = tw2px(doc.page.pageWidthTw);
    const qreal pageH = tw2px(doc.page.pageHeightTw);
    const qreal mT = tw2px(doc.page.marginTopTw), mB = tw2px(doc.page.marginBottomTw);
    const qreal mL = tw2px(doc.page.marginLeftTw), mR = tw2px(doc.page.marginRightTw);
    const qreal bodyW = std::max(40.0, pageW - mL - mR);
    const qreal bodyH = std::max(40.0, pageH - mT - mB);
    QFont base(QStringLiteral("Microsoft YaHei"));
    base.setPointSizeF(10.5);

    // 页码字体
    QFont pnFont = base;
    pnFont.setPointSizeF(9);

    int pageNo = 0;
    qreal y = 0;
    auto startPage = [&]() { ++pageNo; y = 0; };
    auto endPage = [&]() {
        painter.setFont(pnFont);
        painter.setPen(QColor(0x88, 0x8A, 0x90));
        painter.drawText(QRectF(mL, pageH - mB + 2, bodyW, 14), Qt::AlignHCenter,
                         QObject::tr("第 %1 页").arg(pageNo));
    };

    startPage();
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (const auto& node : doc.body) {
        if (node.kind == oc::Node::Kind::Paragraph) {
            ParaLayout pl = layoutPara(node.paragraph, bodyW);
            if (y + pl.h > bodyH && y > 0) {
                endPage();
                if (!painter.isActive()) break;
                if (!dev->newPage()) { if (err) *err = "newPage failed"; return false; }
                startPage();
            }
            painter.save();
            painter.translate(mL, mT + y);
            pl.layout->draw(&painter, QPointF(0, 0));
            painter.restore();
            y += pl.h;
        } else {
            const auto& t = node.table;
            const size_t cols = std::max<size_t>(1, t.colWidthsDxa.size());
            const qreal cw = bodyW / (qreal)cols;
            qreal ty = mT + y;
            // 行高：逐行画（每格最多画 3 行文本高度）
            for (size_t r = 0; r < t.rows.size(); ++r) {
                qreal rowH = 24;
                // 每行换页判断
                if (ty + rowH > pageH - mB && y > 0) {
                    endPage();
                    if (!painter.isActive()) break;
                    if (!dev->newPage()) return false;
                    startPage();
                    ty = mT;
                }
                painter.setPen(QPen(QColor(0xB8, 0xC4, 0xD4), 1));
                painter.drawRect(QRectF(mL, ty, bodyW, rowH));
                for (size_t c = 0; c < t.rows[r].cells.size(); ++c) {
                    QString text;
                    for (const auto& p : t.rows[r].cells[c].paragraphs)
                        for (const auto& run : p.runs)
                            text += QString::fromStdString(run.text);
                    if (r == 0)
                        painter.fillRect(QRectF(mL + c * cw + 1, ty + 1, cw - 2, rowH - 2),
                                         QColor(0xEA, 0xF1, 0xF9));
                    painter.setFont(base);
                    painter.setPen(Qt::black);
                    painter.drawText(QRectF(mL + c * cw + 3, ty, cw - 6, rowH),
                                     Qt::AlignVCenter | Qt::AlignLeft,
                                     text.left(60));
                }
                ty += rowH;
            }
            y = ty - mT + 8;
        }
    }
    endPage();
    painter.end();
    return true;
}

} // namespace oc::qt
