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
        bool bold = false, italic = false, underline = false;
        double sizePt = 0;               // 0 = 混合/默认
        QString eastAsia;                // 空 = 混合/默认
        QString styleId;
        int align = 0;                   // 0左 1中 2右 3两端
    };
    FormatHints formatHints() const;
    bool italicSelection(bool val);
    bool underlineSelection(bool val);
    bool fontSizeSelection(double pt);
    bool fontNameSelection(const QString& family);
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
        qreal y = 0, h = 0;             // 文档流纵坐标
        QVector<QTextLine> lines;
        QFont font;
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

    std::shared_ptr<const oc::Document> doc_;
    oc::DocumentEditor* ed_ = nullptr;
    oc::CommandStack* cs_ = nullptr;

    // 布局（简单起见：全量缓存向量——标书万段级 QTextLayout 内存可控于视口外段落退化）
    std::vector<ParaBox> boxes_;
    qreal docHeight_ = 0;
    int textMarginX_ = 48, topPad_ = 24, lineGap_ = 4, paraGap_ = 10;

    // 光标与选区
    DocPos cursor_{};
    DocPos anchor_{};                   // 选区锚点（anchor==cursor 无选区）
    bool cursorVisible_ = true;
    int blinkTimer_ = 0;
    bool mouseDragging_ = false;
};

} // namespace oc::qt
