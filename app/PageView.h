// oc::qt::PageView — 可编辑文档视图（M2.5）
// 连续滚动流模式（Word Web 版风格）：真实断行排版 + 光标 + 键盘编辑。
// 设计：
//  1. 每段独立 QTextLayout 精确断行（标题/正文样式区分、段间距）；
//  2. 视口虚拟化：只布局可视区 ±缓冲的段落；
//  3. 点击定位光标 / 键盘编辑（输入/Backspace/Delete/Enter/方向键）；
//  4. 中文输入（inputMethod commit）；编辑走 DocumentEditor（全部可撤销）。
#pragma once
#include "../core/Clipboard.h"
#include "../core/Document.h"
#include "../core/EditCommands.h"
#include "../core/ImageCache.h"
#include <QAbstractScrollArea>
#include <QTextLayout>
#include <memory>

namespace oc::qt {

class PageView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PageView(QWidget* parent = nullptr);

    void setDocument(std::shared_ptr<const oc::Document> doc);
    // 编辑上下文（MainWindow 持有 doc/stack/editor 并注入）
    void attachEditor(oc::DocumentEditor* ed, oc::CommandStack* cs);
    void attachImageCache(oc::ImageCache* cache);   // 图片按需解码+LRU

    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    // 选区操作（返回 false = 无选区）
    bool boldSelection(bool val);
    bool selectionBoldHint() const;      // 当前光标/选区加粗态（工具栏提示）
    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void selectAll();
    // 光标跳转
    void moveCursorToStart();

signals:
    void edited();                       // 任何编辑后发射（标题 dirty、状态更新）
    void cursorMoved();                  // 光标/选区变化（工具栏状态同步）

public:
    // 光标/选区格式提示（工具栏同步用）
    struct FormatHints {
        bool hasDoc = false;
        bool bold = false, italic = false, underline = false, strike = false;
        double sizePt = 0;               // 0 = 混合/默认
        QString eastAsia;                // 空 = 混合/默认
        QString styleId, color, highlight;
        int align = 0;                   // 0左 1中 2右 3两端
    };
    FormatHints formatHints() const;
    bool italicSelection(bool val);
    bool underlineSelection(bool val);
    bool fontSizeSelection(double pt);
    bool fontNameSelection(const QString& family);
    bool strikeSelection(bool val);
    bool textColorSelection(const QString& hexRgb);
    bool highlightSelection(const QString& colorName);
    // 批次B：行距/缩进/列表（作用于光标段或选区段）
    bool lineSpacingSelection(double factor);          // 1.0/1.5/2.0…
    bool indentSelection(int deltaChars);              // 左缩进 ±（1字符=240twips）
    bool firstLineIndentSelection();                   // 首行缩进 2 字符切换
    bool toggleList(bool numbered);                    // 圆点/编号列表（前缀式）
    bool paraStyleSelection(const QString& styleId);
    bool alignSelection(int align);      // 0左 1中 2右 3两端（光标所在段）

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override;
    void timerEvent(QTimerEvent*) override;

private:
    struct ParaBox {                    // 段落布局缓存
        int flat = -1;
        std::unique_ptr<QTextLayout> layout;
        qreal y = 0, h = 0, x = 0;      // x：文本绘制左缘（表格内段落有列偏移）
        QVector<QTextLine> lines;
        QFont font;
        struct Img { QString relId; qreal w = 0, h = 0; };
        QVector<Img> imgs;              // 段内图片（真实显示）
        qreal imgBlockH = 0;            // 图片区高度（文本前置）
        qreal textX = 0, textW = 0;     // 缩进后文本区（x 相对文档）
        qreal firstLineIndent = 0;      // 首行缩进 px
        qreal lineFactor = 1.0;         // 行距倍数
    };
    struct TableBox {                   // 表格（真机战报 #3：WPS 可见表格必须渲染）
        qreal y = 0, h = 0, w = 0;
        std::vector<qreal> colX, colW;  // 列起点/列宽
        std::vector<int> headerParas;   // 首行段落（浅蓝底）
        std::vector<int> allParas;      // 全部 cell 段落 flat
    };
    void rebuildLayout();               // 全量布局（编辑后调用；千段级毫秒）
    void updateScrollRange();           // 滚动条范围同步
    void ensureVisible(qreal docY);     // 滚动到文档坐标
    int hitPara(qreal docY) const;      // y → 段索引（-1 无）
    DocPos hitPos(const QPointF& docPt) const;   // 点 → DocPos
    qreal cursorY() const;              // 光标纵坐标（滚动用）
    void updateCursorBlink();
    DocPos endOfPara(size_t flat) const;
    void relayoutAndKeepCursor();
    QString paraText(size_t flat) const;
    // 大纲导航/插入辅助
public:
    void scrollToPara(size_t flat);
    // 批次D：格式刷（复制当前光标格式→点击目标段应用）
    void startFormatBrush();
    bool brushActive() const { return brushActive_; }
    // 查找全部高亮（字符级黄底；空串清除）
    void highlightAllMatches(const QString& needle);
    // 批次E：缩放（Ctrl+滚轮；0.5~4.0）与页视图数据
    double zoom() const { return zoom_; }
    void setZoom(double z);                    // 程序设置（菜单）
    int pageCount() const { return (int)pageBreaks_.size() + 1; }
    int pageOfFlat(size_t flat) const;                  // 段落→页号（目录用）
    qreal pageVisualHeight() const;            // 单页视觉高（含边距×zoom）
    size_t cursorPara() const { return cursor_.para; }
    void refreshAfterEdit() { relayoutAndKeepCursor(); }
    void pasteTextAtCursor(const QString& text);
    // 查找替换（大小写不敏感；findNext 命中跳转并选中）
    bool findNext(const QString& needle);
    int replaceAll(const QString& from, const QString& to);   // 返回替换次数
    void pasteText(const QString& text);                      // 替换当前选中后插入

signals:
    void statsChanged();
private:
    std::vector<ParaBox> boxes_;
    std::vector<TableBox> tables_;
    qreal docHeight_ = 0;
    int textMarginX_ = 48, topPad_ = 24, lineGap_ = 4, paraGap_ = 10;

    // 光标与选区
    DocPos cursor_{};
    DocPos anchor_{};                   // 选区锚点（anchor==cursor 无选区）
    bool cursorVisible_ = true;
    int blinkTimer_ = 0;
    bool mouseDragging_ = false;

    // 文档与编辑上下文
    std::shared_ptr<const oc::Document> doc_;
    oc::DocumentEditor* ed_ = nullptr;
    oc::CommandStack* cs_ = nullptr;
    oc::ImageCache* cache_ = nullptr;   // 图片缓存（MainWindow 持有）
    DocPos charToDocPos(size_t flat, int charIdx) const;  // 字符下标→run/字节偏移
    QString numberedPrefixOf(size_t flat) const;          // 编号列表当前序号前缀
    // 格式刷
    bool brushActive_ = false;
    oc::TextRun brushFmt_;
    // 查找高亮命中（段落字符区间）
    struct Match { size_t flat; int start, len; };
    std::vector<Match> matches_;
    // 批次G：图片选择/拖拽
    size_t imgSelPara_ = (size_t)-1;
    bool imgDragging_ = false;
    QPointF imgDragStart_;
    // 批次E：缩放与分页视图
    double zoom_ = 1.0;                        // 0.5~4.0
    std::vector<qreal> pageBreaks_;            // 每页内容起点 y（页0 从 topPad 起）
    qreal pageW_ = 794, pageH_ = 1123;         // A4 px@96（按文档 sectPr）
    qreal marginTop_ = 96, marginBottom_ = 96, marginLeft_ = 120, marginRight_ = 120;
    int rulerH_ = 22;                          // 水平标尺高
protected:
    void wheelEvent(QWheelEvent*) override;
signals:
    void zoomChanged(double z);
};

} // namespace oc::qt
