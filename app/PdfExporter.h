// oc::qt::PdfExporter — PDF 导出 / 打印（共用绘制路径，WYSIWYG 分页）
// 支持正文（QTextLayout 断行）/ 富格式（B I U S 颜色 高亮 字号 字体）/
// 图片（ImageCache 按需解码）/ 表格网格 / 页脚页码；段落不跨页。
#pragma once
#include "../core/Document.h"
#include "../core/ImageCache.h"
#include <QString>
#include <QFileInfo>

class QPagedPaintDevice;
class QPrinter;

namespace oc::qt {

class PdfExporter {
public:
    // 导出到 PDF 文件
    static bool exportPdf(const QString& pdfPath, const oc::Document& doc,
                          oc::ImageCache* cache, QString* err = nullptr);
    // 导出到打印机（弹出打印对话框；用户取消返回 false 且 *err 空）
    static bool printDialog(class QWidget* parent, const oc::Document& doc,
                            oc::ImageCache* cache, QString* err = nullptr);
    // 共用：逐页绘制到 QPaintDevice（QPdfWriter / QPrinter）
    static bool paintPages(class QPagedPaintDevice* dev, const oc::Document& doc,
                           oc::ImageCache* cache, QString* err = nullptr);
};

} // namespace oc::qt
