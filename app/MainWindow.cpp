#include "MainWindow.h"
#include "FindReplace.h"
#include "LogPanel.h"
#include "PageView.h"
#include "PdfExporter.h"
#include "../core/CommandStack.h"
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/EditCommands.h"
#include "../core/FontCheck.h"
#include "../core/ImageCache.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QPrinter>
#include <QInputDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QIcon>
#include <QPixmap>
#include <QFontDatabase>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolBar>
#include <QTreeWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QDate>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QFile>
#include <QSpinBox>
#include <QTimer>
#include <chrono>
#include <map>
#include <thread>

namespace oc::qt {

ZipReader* MainWindow::s_srcZip = nullptr;
const std::map<QString, std::vector<uint8_t>>* MainWindow::s_newMedia = nullptr;

MainWindow::MainWindow() {
    setWindowTitle(tr("Office Clone — Word"));
    resize(1280, 840);

    view_ = new PageView(this);
    logPanel_ = new LogPanel(this);
    infoCard_ = new QLabel(this);
    infoCard_->setWordWrap(true);
    infoCard_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    infoCard_->setFrameShape(QFrame::StyledPanel);
    infoCard_->setText(tr("文档速览\n\n尚未打开文档\nCtrl+O 打开 .docx"));

    // 右侧栏：上=文档速览卡，下=日志框（用户要求的右下角日志区）
    auto* rightSplit = new QSplitter(Qt::Vertical, this);
    rightSplit->addWidget(infoCard_);
    rightSplit->addWidget(logPanel_);
    rightSplit->setStretchFactor(0, 1);
    rightSplit->setStretchFactor(1, 2);
    rightSplit->setSizes({140, 320});

    outlineTree_ = new QTreeWidget(this);
    outlineTree_->setHeaderLabel(tr("目录"));
    outlineTree_->setMinimumWidth(150);
    connect(outlineTree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                bool ok = false;
                size_t flat = item->data(0, Qt::UserRole).toULongLong(&ok);
                if (ok) view_->scrollToPara(flat);
            });

    auto* mainSplit = new QSplitter(Qt::Horizontal, this);
    mainSplit->addWidget(outlineTree_);
    mainSplit->addWidget(view_);
    mainSplit->addWidget(rightSplit);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setStretchFactor(2, 0);
    mainSplit->setSizes({200, 900, 320});
    setCentralWidget(mainSplit);

    status_ = new QLabel(tr("就绪"), this);
    statusBar()->addWidget(status_, 1);
    wordCount_ = new QLabel(tr("字数：0"), this);
    statusBar()->addPermanentWidget(wordCount_);

    findDlg_ = new FindReplaceDialog(view_, this);
    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setInterval(30 * 1000);           // 30s 无操作自动保存
    autosaveTimer_->setSingleShot(true);
    connect(autosaveTimer_, &QTimer::timeout, this, [this]() {
        if (dirty_ && !openPath_.isEmpty()) {
            logPanel_->log(QStringLiteral("INFO"), tr("自动保存触发…"));
            onSave();
        }
    });

    buildMenu();
    buildToolbar();
    connect(view_, &PageView::edited, this, &MainWindow::onEdited);
    connect(view_, &PageView::cursorMoved, this, &MainWindow::syncToolbar);
    logPanel_->log(QStringLiteral("INFO"),
                   tr("office-clone 测试版启动。支持：点击定位、键盘输入、中文输入、"
                      "Backspace/Delete、回车拆段、Ctrl+Z/Y 撤销重做、Ctrl+B 加粗、"
                      "Ctrl+C/V/X 剪贴板、Ctrl+S 保存。"));
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenu() {
    QMenu* file = menuBar()->addMenu(tr("文件(&F)"));
    QAction* neew = file->addAction(tr("新建(&N)"));
    neew->setShortcut(QKeySequence::New);
    connect(neew, &QAction::triggered, this, [this]() {
        if (dirty_) {
            auto b = QMessageBox::question(this, tr("未保存的修改"),
                                           tr("放弃当前修改并新建文档？"));
            if (b != QMessageBox::Yes) return;
        }
        auto d = std::make_shared<oc::Document>();
        d->page = oc::PageSetup{};                     // A4 默认
        d->body.emplace_back(oc::Node{oc::Node::Kind::Paragraph, oc::Paragraph{}, oc::Table{}});
        openPath_.clear();
        onLoaded(d, 0, QString(), {});
        logPanel_->log(QStringLiteral("INFO"), tr("新建空白文档（未保存，Ctrl+S 时选择位置）"));
    });
    QAction* open = file->addAction(tr("打开(&O)..."));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::onOpenDialog);
    QAction* save = file->addAction(tr("保存(&S)"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &MainWindow::onSave);
    QAction* saveAs = file->addAction(tr("另存为(&A)..."));
    connect(saveAs, &QAction::triggered, this, &MainWindow::onSaveAs);
    QAction* pdf = file->addAction(tr("导出 PDF(&E)..."));
    pdf->setShortcut(tr("Ctrl+E"));
    connect(pdf, &QAction::triggered, this, [this]() {
        if (!doc_) return;
        QString path = QFileDialog::getSaveFileName(this, tr("导出 PDF"),
            QFileInfo(openPath_).absolutePath() + "/" +
            QFileInfo(openPath_).completeBaseName() + ".pdf",
            tr("PDF 文档 (*.pdf)"));
        if (path.isEmpty()) return;
        QString err;
        logPanel_->log(QStringLiteral("INFO"), tr("正在导出 PDF：%1 …").arg(path));
        auto t0 = std::chrono::steady_clock::now();
        bool ok = PdfExporter::exportPdf(path, *doc_, imageCache_.get(), &err);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (ok) logPanel_->log(QStringLiteral("INFO"),
                    tr("PDF 导出成功：%1 ｜ 耗时 %2 s").arg(path).arg(sec, 0, 'f', 1));
        else logPanel_->log(QStringLiteral("ERROR"), tr("PDF 导出失败：%1").arg(err));
    });
    QAction* prn = file->addAction(tr("打印(&P)..."));
    prn->setShortcut(QKeySequence::Print);
    connect(prn, &QAction::triggered, this, [this]() {
        if (!doc_) return;
        QString err;
        if (!PdfExporter::printDialog(this, *doc_, imageCache_.get(), &err) && !err.isEmpty())
            logPanel_->log(QStringLiteral("ERROR"), tr("打印失败：%1").arg(err));
    });
    file->addSeparator();
    QAction* quit = file->addAction(tr("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("编辑(&E)"));
    QAction* findAct = edit->addAction(tr("查找(&F)..."));
    findAct->setShortcut(QKeySequence::Find);
    connect(findAct, &QAction::triggered, this, [this]() { findDlg_->showFind(); });
    QAction* repAct = edit->addAction(tr("替换(&R)..."));
    repAct->setShortcut(QKeySequence::Replace);
    connect(repAct, &QAction::triggered, this, [this]() { findDlg_->showReplace(); });
    QAction* autosave = edit->addAction(tr("自动保存（静默30秒）"));
    autosave->setCheckable(true);
    autosave->setChecked(true);
    connect(autosave, &QAction::toggled, this, [this](bool on) {
        if (!on) autosaveTimer_->stop();
        logPanel_->log(QStringLiteral("INFO"),
                       tr("自动保存已%1").arg(on ? tr("开启") : tr("关闭")));
    });
    edit->addSeparator();
    auto addEdit = [&](const QString& text, QKeySequence::StandardKey key,
                       const char* slot) {
        QAction* a = edit->addAction(text);
        a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, slot]() {
            if (slot == QStringLiteral("undo")) view_->undo();
            else if (slot == QStringLiteral("redo")) view_->redo();
            else if (slot == QStringLiteral("copy")) view_->copySelection();
            else if (slot == QStringLiteral("cut")) view_->cutSelection();
            else if (slot == QStringLiteral("paste")) view_->pasteClipboard();
            else if (slot == QStringLiteral("selectall")) view_->selectAll();
            else if (slot == QStringLiteral("bold")) view_->boldSelection(!view_->selectionBoldHint());
        });
    };
    addEdit(tr("撤销(&U)"), QKeySequence::Undo, "undo");
    addEdit(tr("重做(&R)"), QKeySequence::Redo, "redo");
    edit->addSeparator();
    addEdit(tr("复制(&C)"), QKeySequence::Copy, "copy");
    addEdit(tr("剪切(&T)"), QKeySequence::Cut, "cut");
    addEdit(tr("粘贴(&P)"), QKeySequence::Paste, "paste");
    addEdit(tr("全选(&A)"), QKeySequence::SelectAll, "selectall");
    edit->addSeparator();
    QAction* pastePlain = edit->addAction(tr("选择性粘贴(&V)（仅文本）"));
    pastePlain->setShortcut(tr("Ctrl+Shift+V"));
    connect(pastePlain, &QAction::triggered, this, [this]() {
        view_->pasteClipboard();                        // 剪贴板→纯文本路径（无格式）
    });
    QAction* gotoAct = edit->addAction(tr("定位(&G)..."));
    gotoAct->setShortcut(tr("Ctrl+G"));
    connect(gotoAct, &QAction::triggered, this, [this]() {
        bool ok = false;
        int n = QInputDialog::getInt(this, tr("定位到段落"), tr("段落号（1 起）："),
                                     1, 1, 1000000, 1, &ok);
        if (ok) view_->scrollToPara((size_t)(n - 1));
    });
    QMenu* ins = menuBar()->addMenu(tr("插入(&I)"));
    QAction* insImg = ins->addAction(tr("图片(&P)..."));
    connect(insImg, &QAction::triggered, this, [this]() { onInsertImage(); });
    QAction* insTbl = ins->addAction(tr("表格(&T)..."));
    connect(insTbl, &QAction::triggered, this, [this]() { onInsertTable(); });
    QAction* insPB = ins->addAction(tr("分页符(&B)（本段前分页，点击切换）"));
    connect(insPB, &QAction::triggered, this, [this]() {
        if (!editor_) return;
        const oc::Paragraph* p = editor_->paragraphAt(view_->cursorPara());
        bool cur = p && p->pageBreakBefore;
        editor_->setPageBreakBefore(view_->cursorPara(), !cur);
        view_->refreshAfterEdit();
    });
    QAction* insTOC = ins->addAction(tr("目录(&C)（从标题生成）"));
    connect(insTOC, &QAction::triggered, this, [this]() {
        if (!doc_ || !editor_) return;
        std::vector<oc::Paragraph> tocs;
        oc::Paragraph title;
        title.styleId = "Title"; title.align = oc::Align::Center;
        oc::TextRun titleRun; titleRun.text = "目 录"; title.runs = {titleRun};
        tocs.push_back(title);
        const size_t n = editor_->paragraphCount();
        for (size_t i = 0; i < n; ++i) {
            const oc::Paragraph* p = editor_->paragraphAt(i);
            if (!p || (p->styleId != "Heading1" && p->styleId != "Heading2" &&
                       p->styleId != "Heading3" && p->styleId != "Title"))
                continue;
            if (p->styleId == "Title") continue;        // 文档标题不入目录
            QString text;
            for (const auto& r : p->runs) text += QString::fromStdString(r.text);
            oc::Paragraph line;
            line.styleId = p->styleId;
            oc::TextRun lr;
            int lvl = p->styleId == "Heading1" ? 0 : p->styleId == "Heading2" ? 1 : 2;
            lr.text = std::string(lvl * 2, ' ') + text.toStdString() + " ……… " +
                      std::to_string(view_->pageOfFlat(i) + 1);
            line.runs = {lr};
            tocs.push_back(line);
        }
        if (tocs.size() <= 1) {
            logPanel_->log(QStringLiteral("WARN"), tr("未发现标题段（先设置标题1/2/3）"));
            return;
        }
        editor_->insertParagraphsAtStart(tocs);
        view_->refreshAfterEdit();
        logPanel_->log(QStringLiteral("INFO"), tr("已生成目录（%1 条）").arg(tocs.size() - 1));
    });
    QAction* insDate = ins->addAction(tr("日期和时间(&D)"));
    connect(insDate, &QAction::triggered, this, [this]() {
        QString d = QDate::currentDate().toString(tr("yyyy年M月d日"));
        view_->pasteTextAtCursor(d);
    });

    QMenu* tblMenu = menuBar()->addMenu(tr("表格(&B)"));
    auto addTblOp = [&](const QString& text, auto fn) {
        QAction* a = tblMenu->addAction(text);
        connect(a, &QAction::triggered, this, [this, fn]() {
            if (editor_ && fn(view_->cursorPara())) { view_->refreshAfterEdit(); emit view_->edited(); }
            else logPanel_->log(QStringLiteral("WARN"), tr("光标需在表格内"));
        });
    };
    addTblOp(tr("在下方插入行"), [this](size_t fp) { return editor_->tableInsertRowAfter(fp); });
    addTblOp(tr("删除本行"),     [this](size_t fp) { return editor_->tableDeleteRow(fp); });
    addTblOp(tr("右侧插入列"),   [this](size_t fp) { return editor_->tableInsertColRight(fp); });
    addTblOp(tr("删除末列"),     [this](size_t fp) { return editor_->tableDeleteCol(fp); });
    QAction* bold = edit->addAction(tr("加粗(&B)"));
    bold->setShortcut(QKeySequence::Bold);
    connect(bold, &QAction::triggered, this, [this]() {
        view_->boldSelection(!view_->selectionBoldHint());
    });
    QAction* italicAct = edit->addAction(tr("斜体(&I)"));
    italicAct->setShortcut(QKeySequence::Italic);
    connect(italicAct, &QAction::triggered, this, [this]() {
        view_->italicSelection(!view_->formatHints().italic);
    });
    QAction* ulAct = edit->addAction(tr("下划线(&D)"));
    ulAct->setShortcut(QKeySequence::Underline);
    connect(ulAct, &QAction::triggered, this, [this]() {
        view_->underlineSelection(!view_->formatHints().underline);
    });
}

void MainWindow::buildToolbar() {
    auto* tb = addToolBar(tr("格式(&F)"));
    tb->setMovable(false);

    // 样式（正文/标题）
    styleCombo_ = new QComboBox(this);
    styleCombo_->addItem(tr("正文"), QStringLiteral(""));
    styleCombo_->addItem(tr("标题1"), QStringLiteral("Heading1"));
    styleCombo_->addItem(tr("标题2"), QStringLiteral("Heading2"));
    styleCombo_->addItem(tr("标题3"), QStringLiteral("Heading3"));
    styleCombo_->addItem(tr("标题(Title)"), QStringLiteral("Title"));
    styleCombo_->setToolTip(tr("段落样式"));
    tb->addWidget(styleCombo_);
    connect(styleCombo_, qOverload<int>(&QComboBox::activated), this, [this](int) {
        if (!syncingToolbar_)
            view_->paraStyleSelection(styleCombo_->currentData().toString());
    });

    tb->addSeparator();

    // 字体（系统字体族）
    fontCombo_ = new QFontComboBox(this);
    fontCombo_->setToolTip(tr("字体（中西文统一设置）"));
    tb->addWidget(fontCombo_);
    connect(fontCombo_, &QComboBox::textActivated, this, [this](const QString& f) {
        if (!syncingToolbar_) view_->fontNameSelection(f);
    });

    // 中文字号
    sizeCombo_ = new QComboBox(this);
    struct ZH { const char* name; double pt; };
    const ZH zhSizes[] = {
        {"初号", 42}, {"小初", 36}, {"一号", 26}, {"小一", 24}, {"二号", 22},
        {"小二", 18}, {"三号", 16}, {"小三", 15}, {"四号", 14}, {"小四", 12},
        {"五号", 10.5}, {"小五", 9}, {"六号", 7.5},
    };
    for (const auto& z : zhSizes)
        sizeCombo_->addItem(QString::fromUtf8(z.name), z.pt);
    sizeCombo_->setCurrentIndex(9);       // 小四默认
    sizeCombo_->setToolTip(tr("字号（中文字号）"));
    tb->addWidget(sizeCombo_);
    // 字号增大/减小（WPS 风格）
    auto* grow = new QToolButton(this);
    grow->setText(tr("A⁺"));
    grow->setToolTip(tr("增大字号"));
    grow->setAutoRaise(true);
    tb->addWidget(grow);
    auto* shrink = new QToolButton(this);
    shrink->setText(tr("A⁻"));
    shrink->setToolTip(tr("减小字号"));
    shrink->setAutoRaise(true);
    tb->addWidget(shrink);
    auto adjustSize = [this](double delta) {
        auto h = view_->formatHints();
        double cur = h.sizePt > 0 ? h.sizePt : 10.5;
        view_->fontSizeSelection(qBound(6.0, cur + delta, 72.0));
    };
    connect(grow, &QToolButton::clicked, this, [adjustSize]() { adjustSize(2.0); });
    connect(shrink, &QToolButton::clicked, this, [adjustSize]() { adjustSize(-2.0); });
    connect(sizeCombo_, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        if (!syncingToolbar_)
            view_->fontSizeSelection(sizeCombo_->itemData(idx).toDouble());
    });

    tb->addSeparator();

    // B / I / U
    auto makeBtn = [&](const QString& text, const QString& tip, QKeySequence::StandardKey key) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        QFont f = b->font();
        f.setBold(text == tr("B"));
        f.setItalic(text == tr("I"));
        f.setUnderline(text == tr("U"));
        b->setFont(f);
        tb->addWidget(b);
        return b;
    };
    boldBtn_ = makeBtn(tr("B"), tr("加粗 Ctrl+B"), QKeySequence::Bold);
    italicBtn_ = makeBtn(tr("I"), tr("斜体 Ctrl+I"), QKeySequence::Italic);
    underlineBtn_ = makeBtn(tr("U"), tr("下划线 Ctrl+U"), QKeySequence::Underline);
    strikeBtn_ = makeBtn(tr("S"), tr("删除线"), QKeySequence::UnknownKey);
    {
        QFont sf = strikeBtn_->font();
        sf.setStrikeOut(true);
        strikeBtn_->setFont(sf);
    }
    auto connFormat = [&](QToolButton* b, auto fn) {
        connect(b, &QToolButton::clicked, this, [this, fn, b]() {
            if (!syncingToolbar_ && view_) fn(b->isChecked());
        });
    };
    connFormat(boldBtn_, [this](bool v) { view_->boldSelection(v); });
    connFormat(italicBtn_, [this](bool v) { view_->italicSelection(v); });
    connFormat(underlineBtn_, [this](bool v) { view_->underlineSelection(v); });
    connFormat(strikeBtn_, [this](bool v) { view_->strikeSelection(v); });
    auto* brushBtn = new QToolButton(this);
    brushBtn->setText(tr("🖌"));
    brushBtn->setToolTip(tr("格式刷：光标放到源格式处→点此→点击目标段落"));
    brushBtn->setAutoRaise(true);
    tb->addWidget(brushBtn);
    connect(brushBtn, &QToolButton::clicked, this, [brushBtn, this]() {
        view_->startFormatBrush();
        brushBtn->setChecked(view_->brushActive());
    });

    // 文字颜色（标准色板）
    colorCombo_ = new QComboBox(this);
    struct CItem { const char* name; const char* hex; const char* disp; };
    const CItem colors[] = {
        {"", "000000", "自动"}, {"FF0000", "FF0000", "红"}, {"FFC000", "FFC000", "橙"},
        {"FFFF00", "FFFF00", "黄"}, {"92D050", "92D050", "绿"}, {"00B0F0", "00B0F0", "蓝"},
        {"0070C0", "0070C0", "深蓝"}, {"7030A0", "7030A0", "紫"}, {"C00000", "C00000", "深红"},
        {"000000", "000000", "黑"},
    };
    for (const auto& c : colors) {
        QPixmap pm(14, 14);
        pm.fill(QColor(QString::fromLatin1(c.hex)));
        QIcon icon(pm);
        colorCombo_->addItem(icon, QString::fromUtf8(c.disp), QString::fromLatin1(c.hex));
    }
    colorCombo_->setToolTip(tr("文字颜色"));
    tb->addWidget(colorCombo_);
    connect(colorCombo_, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        if (!syncingToolbar_)
            view_->textColorSelection(colorCombo_->itemData(idx).toString());
    });

    // 字符高亮（色板）
    hlCombo_ = new QComboBox(this);
    const CItem hls[] = {
        {"", "", "无"}, {"yellow", "FFE100", "黄"}, {"bright_green", "00FF00", "绿"},
        {"cyan", "00FFFF", "青"}, {"magenta", "FF00FF", "粉"}, {"red", "FF0000", "红"},
        {"blue", "00B0F0", "蓝"},
    };
    for (const auto& c : hls) {
        QPixmap pm(14, 14);
        if (c.hex[0]) pm.fill(QColor(QString::fromLatin1(c.hex)));
        else pm.fill(Qt::transparent);
        hlCombo_->addItem(QIcon(pm), QString::fromUtf8(c.disp), QString::fromLatin1(c.name));
    }
    hlCombo_->setToolTip(tr("字符高亮"));
    tb->addWidget(hlCombo_);
    connect(hlCombo_, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        if (!syncingToolbar_)
            view_->highlightSelection(hlCombo_->itemData(idx).toString());
    });

    tb->addSeparator();

    // 批次B：行距下拉
    auto* lineCombo = new QComboBox(this);
    struct LS { const char* name; double f; };
    const LS lss[] = {{"1.0", 1.0}, {"1.15", 1.15}, {"1.5", 1.5}, {"2.0", 2.0}, {"2.5", 2.5}, {"3.0", 3.0}};
    for (const auto& l : lss) lineCombo->addItem(QString::fromUtf8(l.name), l.f);
    lineCombo->setToolTip(tr("行距"));
    tb->addWidget(lineCombo);
    connect(lineCombo, qOverload<int>(&QComboBox::activated), this, [this, lineCombo](int idx) {
        if (!syncingToolbar_)
            view_->lineSpacingSelection(lineCombo->itemData(idx).toDouble());
    });

    // 缩进：减少/增加/首行
    auto* indLess = new QToolButton(this);
    indLess->setText(tr("⇤"));
    indLess->setToolTip(tr("减少缩进"));
    indLess->setAutoRaise(true);
    tb->addWidget(indLess);
    connect(indLess, &QToolButton::clicked, this, [this]() { view_->indentSelection(-1); });
    auto* indMore = new QToolButton(this);
    indMore->setText(tr("⇥"));
    indMore->setToolTip(tr("增加缩进"));
    indMore->setAutoRaise(true);
    tb->addWidget(indMore);
    connect(indMore, &QToolButton::clicked, this, [this]() { view_->indentSelection(1); });
    auto* indFirst = new QToolButton(this);
    indFirst->setText(tr("⇥̄"));
    indFirst->setToolTip(tr("首行缩进（2字符，点击切换）"));
    indFirst->setAutoRaise(true);
    tb->addWidget(indFirst);
    connect(indFirst, &QToolButton::clicked, this, [this]() { view_->firstLineIndentSelection(); });

    tb->addSeparator();

    // 列表：圆点/编号
    auto* bulletBtn = new QToolButton(this);
    bulletBtn->setText(tr("•≡"));
    bulletBtn->setToolTip(tr("项目符号列表（回车自动续）"));
    bulletBtn->setAutoRaise(true);
    tb->addWidget(bulletBtn);
    connect(bulletBtn, &QToolButton::clicked, this, [this]() { view_->toggleList(false); });
    auto* numBtn = new QToolButton(this);
    numBtn->setText(tr("1.≡"));
    numBtn->setToolTip(tr("编号列表（回车自动递增）"));
    numBtn->setAutoRaise(true);
    tb->addWidget(numBtn);
    connect(numBtn, &QToolButton::clicked, this, [this]() { view_->toggleList(true); });

    tb->addSeparator();

    // 对齐（互斥）
    alignGroup_ = new QActionGroup(this);
    alignActs_.clear();
    auto addAlign = [&](const QString& icon, int a, const QString& tip) {
        QAction* act = alignGroup_->addAction(icon);
        act->setCheckable(true);
        act->setToolTip(tip);
        tb->addAction(act);
        connect(act, &QAction::triggered, this, [this, a]() {
            if (!syncingToolbar_ && view_) view_->alignSelection(a);
        });
        alignActs_.append(act);
    };
    addAlign(tr("⟵"), 0, tr("左对齐"));
    addAlign(tr("⟶"), 1, tr("居中"));
    addAlign(tr("⟶|"), 2, tr("右对齐"));
    addAlign(tr("☰"), 3, tr("两端对齐"));
    alignActs_[0]->setChecked(true);
}

void MainWindow::syncToolbar() {
    if (!view_) return;
    auto h = view_->formatHints();
    syncingToolbar_ = true;
    boldBtn_->setChecked(h.bold);
    italicBtn_->setChecked(h.italic);
    underlineBtn_->setChecked(h.underline);
    strikeBtn_->setChecked(h.strike);
    // 对齐
    if (h.align >= 0 && h.align < alignActs_.size())
        alignActs_[h.align]->setChecked(true);
    // 样式
    int idx = styleCombo_->findData(h.styleId);
    if (idx >= 0) styleCombo_->setCurrentIndex(idx);
    // 字号：找最接近的中文号
    if (h.sizePt > 0) {
        int best = 0;
        double bestD = 1e9;
        for (int i = 0; i < sizeCombo_->count(); ++i) {
            double d = qAbs(sizeCombo_->itemData(i).toDouble() - h.sizePt);
            if (d < bestD) { bestD = d; best = i; }
        }
        sizeCombo_->setCurrentIndex(best);
    }
    syncingToolbar_ = false;
}

void MainWindow::onOpenDialog() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("打开 Word 文档"), QString(),
        tr("Word 文档 (*.docx);;所有文件 (*)"));
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::openFile(const QString& path) {
    if (dirty_) {
        auto btn = QMessageBox::question(this, tr("未保存的修改"),
            tr("当前文档有未保存修改，放弃并打开新文件？"));
        if (btn != QMessageBox::Yes) return;
    }
    status_->setText(tr("正在打开：%1 …").arg(path));
    logPanel_->log(QStringLiteral("INFO"), tr("正在打开：%1").arg(path));
    setEnabled(false);

    std::thread([this, path]() {
        auto t0 = std::chrono::steady_clock::now();
        auto doc = std::make_shared<oc::Document>();
        DocxReadResult r = DocxReader::read(path.toStdString(), *doc);
        double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
        QString err = r.ok ? QString() : QString::fromStdString(r.error);
        QMetaObject::invokeMethod(this, [this, doc, ms, err,
                                         warnings = r.warnings]() {
            onLoaded(doc, ms, err, warnings);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::onLoaded(std::shared_ptr<oc::Document> doc, double ms, const QString& err,
                          const std::vector<std::string>& warnings) {
    setEnabled(true);
    if (!err.isEmpty()) {
        status_->setText(tr("打开失败：%1").arg(err));
        logPanel_->log(QStringLiteral("ERROR"), tr("打开失败：%1").arg(err));
        return;
    }
    doc_ = doc;
    openPath_ = QString::fromStdString(doc->sourcePath);
    stack_ = std::make_unique<oc::CommandStack>();
    editor_ = std::make_unique<oc::DocumentEditor>(*doc_, *stack_);
    srcZip_ = std::make_unique<ZipReader>();
    srcZip_->open(openPath_.toStdString());
    imageCache_ = std::make_unique<oc::ImageCache>(256ull * 1024 * 1024);
    imageCache_->setSource(openPath_.toStdString(), doc_.get());
    imageCache_->setExternalDecoder(
        [](const std::vector<uint8_t>& file, oc::DecodedImage& out) -> bool {
            QImage img = QImage::fromData(file.data(), (int)file.size());
            if (img.isNull()) return false;
            QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
            out.width = (uint32_t)rgba.width();
            out.height = (uint32_t)rgba.height();
            out.rgba.assign(rgba.bits(), rgba.bits() + rgba.sizeInBytes());
            return true;
        });
    view_->setDocument(doc_);
    view_->attachEditor(editor_.get(), stack_.get());
    view_->attachImageCache(imageCache_.get());
    view_->setFocus();
    dirty_ = false;

    int slash = openPath_.lastIndexOf('/'); if (slash < 0) slash = openPath_.lastIndexOf('\\');
    setWindowTitle(tr("%1 — Office Clone").arg(openPath_.mid(slash + 1)));
    status_->setText(tr("%1 ｜ 打开 %2 ms").arg(QString::fromStdString(doc->summary()))
                         .arg(ms, 0, 'f', 0));
    infoCard_->setText(tr("📄 %1\n\n%2\n\n打开耗时 %3 ms\n\n提示：点击正文即可编辑；"
                          "Ctrl+S 保存到原文件。")
                           .arg(openPath_.mid(slash + 1))
                           .arg(QString::fromStdString(doc->summary()))
                           .arg(ms, 0, 'f', 0));
    logPanel_->log(QStringLiteral("INFO"), tr("打开成功｜耗时 %1 ms（图片按需解码+LRU）").arg(ms, 0, 'f', 0));

    std::map<QString, int> warnCount;
    for (const auto& w : warnings) ++warnCount[QString::fromStdString(w)];
    for (auto it = warnCount.begin(); it != warnCount.end(); ++it)
        logPanel_->log(QStringLiteral("WARN"), tr("解析提示 ×%1：%2").arg(it->second).arg(it->first));

    auto used = FontCheck::collectUsedFonts(*doc_);
    if (!used.empty()) {
        std::set<std::string> installed;
        for (const QString& f : QFontDatabase::families()) installed.insert(f.toStdString());
        auto issues = FontCheck::check(used, installed);
        if (issues.empty())
            logPanel_->log(QStringLiteral("INFO"),
                           tr("字体检查：%1 种字体全部已安装 ✓").arg(used.size()));
        for (const auto& i : issues)
            logPanel_->log(QStringLiteral("WARN"),
                tr("字体缺失：%1%2").arg(QString::fromStdString(i.missing))
                    .arg(i.fallback.empty() ? tr("（无回退建议）")
                                             : tr("（显示回退 %1）").arg(QString::fromStdString(i.fallback))));
    }
    rebuildOutline();
}

void MainWindow::rebuildOutline() {
    outlineTree_->clear();
    if (!editor_) return;
    QTreeWidgetItem* top = nullptr;
    const size_t n = editor_->paragraphCount();
    for (size_t i = 0; i < n; ++i) {
        const oc::Paragraph* p = editor_->paragraphAt(i);
        if (!p) continue;
        const bool isTitle = p->styleId == "Title";
        int level = 0;
        if (isTitle) level = 1;
        else if (p->styleId == "Heading1") level = 2;
        else if (p->styleId == "Heading2") level = 3;
        else if (p->styleId == "Heading3") level = 4;
        if (!level) continue;
        QString text;
        for (const auto& r : p->runs) text += QString::fromStdString(r.text);
        if (text.isEmpty()) text = tr("（空标题）");
        auto* item = new QTreeWidgetItem(QStringList() << text.left(40));
        item->setData(0, Qt::UserRole, (qulonglong)i);
        if (level == 1 || !top) {
            outlineTree_->addTopLevelItem(item);
            top = item;
        } else {
            top->addChild(item);
        }
    }
    outlineTree_->expandAll();
    logPanel_->log(QStringLiteral("INFO"),
                   tr("目录导航：%1 个标题").arg(outlineTree_->topLevelItemCount()));
}

void MainWindow::onEdited() {
    dirty_ = true;
    autosaveTimer_->start();                          // 静默 30s 后自动保存
    if (doc_) {
        const_cast<oc::Document&>(*doc_).recountStats();
        wordCount_->setText(tr("字数：%1 ｜ 段：%2 ｜ 表：%3")
                                .arg(doc_->stats.unicodeChars)
                                .arg(doc_->stats.paragraphs)
                                .arg(doc_->stats.tables));
    }
    if (!openPath_.isEmpty())
        setWindowTitle(tr("%1* — Office Clone").arg(QFileInfo(openPath_).fileName()));
    status_->setText(tr("已修改（Ctrl+S 保存）｜ %1")
                         .arg(QString::fromStdString(doc_->summary())));
}

bool MainWindow::mediaProvider(const std::string& relId, const std::string& zipPath,
                               std::vector<uint8_t>& out) {
    // 内存新媒体（本会话插入）优先
    if (s_newMedia) {
        auto it = s_newMedia->find(QString::fromStdString(relId));
        if (it != s_newMedia->end()) { out = it->second; return true; }
    }
    (void)zipPath;
    if (!s_srcZip) return false;
    const ZipEntry* e = s_srcZip->find(zipPath);
    return e && s_srcZip->read(*e, out);
}

bool MainWindow::writeDocxTo(const QString& targetPath) {
    if (!doc_) return false;
    // ⚠️ 先关闭源文件长驻句柄——Windows 下自己打开的 rb 句柄会拒绝同一文件的 wb 创建
    //（真机战报 #4：保存报 cannot create zip）
    if (srcZip_ && targetPath == openPath_) srcZip_.reset();
    s_srcZip = srcZip_.get();
    s_newMedia = &newMediaBytes_;
    std::vector<std::string> warnings;
    auto t0 = std::chrono::steady_clock::now();
    DocxWriteResult r = DocxWriter::write(targetPath.toStdString(), *doc_,
                                          &MainWindow::mediaProvider, &warnings);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    s_srcZip = nullptr;
    s_newMedia = nullptr;
    // 保存后重新打开长驻句柄（后续保存的媒体搬运基準）
    if (!srcZip_) {
        srcZip_ = std::make_unique<ZipReader>();
        std::string zerr;
        if (!srcZip_->open(targetPath.toStdString(), &zerr))
            logPanel_->log(QStringLiteral("WARN"),
                           tr("保存后重开源文件失败：%1").arg(QString::fromStdString(zerr)));
    }
    if (!r.ok) {
        logPanel_->log(QStringLiteral("ERROR"),
                       tr("保存失败：%1（若文件正在 Word/WPS 中打开，请先关闭后重试）")
                           .arg(QString::fromStdString(r.error)));
        QMessageBox::warning(this, tr("保存失败"),
            tr("%1\n\n若该文件正在 Word/WPS 中打开，请先关闭后重试；"
               "或使用 文件→另存为 保存到新文件。")
                .arg(QString::fromStdString(r.error)));
        return false;
    }
    dirty_ = false;
    setWindowTitle(tr("%1 — Office Clone").arg(QFileInfo(targetPath).fileName()));
    logPanel_->log(QStringLiteral("INFO"),
                   tr("保存成功：%1 ｜ %2 MB ｜ 耗时 %3 s")
                       .arg(targetPath).arg(r.writtenBytes / 1048576.0, 0, 'f', 1)
                       .arg(ms / 1000.0, 0, 'f', 1));
    status_->setText(tr("已保存 ✓ %1").arg(QFileInfo(targetPath).fileName()));
    return true;
}


void MainWindow::onInsertImage() {
    if (!doc_ || !editor_) return;
    QString path = QFileDialog::getOpenFileName(this, tr("插入图片"), QString(),
        tr("图片 (*.png *.jpg *.jpeg *.bmp *.gif);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        logPanel_->log(QStringLiteral("ERROR"), tr("读图片失败：%1").arg(path));
        return;
    }
    QByteArray bytes = f.readAll();
    QImage img = QImage::fromData(bytes);
    if (img.isNull()) {
        logPanel_->log(QStringLiteral("ERROR"), tr("无法解码图片：%1").arg(path));
        return;
    }
    // 生成 relId 与媒体条目（A4 正文宽内等比）
    QString relId = QStringLiteral("rIdNewImg%1").arg(++newMediaSeq_);
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("jpeg")) ext = QStringLiteral("jpg");
    if (ext.isEmpty()) ext = QStringLiteral("png");
    std::string zipPath = "word/media/" + relId.toStdString() + "." + ext.toStdString();
    MediaEntry me;
    me.relId = relId.toStdString();
    me.zipPath = zipPath;
    me.byteSize = (uint64_t)bytes.size();
    doc_->mediaIndex.push_back(me);                    // const 转换由 editor 路径处理（见下）
    double w = img.width() * 9525.0, h = img.height() * 9525.0;
    const double maxW = 610.0 * 9525.0;                // ≈A4 正文宽
    if (w > maxW) { h = h * maxW / w; w = maxW; }
    size_t curPara = view_->cursorPara();
    editor_->insertImage(curPara, relId.toStdString(),
                         (uint32_t)w, (uint32_t)h);
    if (imageCache_)
        imageCache_->addMemoryMedia(relId.toStdString(),
                                    std::vector<uint8_t>(bytes.begin(), bytes.end()));
    newMediaBytes_[relId] = std::vector<uint8_t>(bytes.begin(), bytes.end());
    view_->refreshAfterEdit();
    logPanel_->log(QStringLiteral("INFO"),
                   tr("插入图片：%1（%2×%3 px）")
                       .arg(QFileInfo(path).fileName()).arg(img.width()).arg(img.height()));
}

void MainWindow::onInsertTable() {
    if (!doc_ || !editor_) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("插入表格"));
    auto* rows = new QSpinBox(&dlg); rows->setRange(1, 100); rows->setValue(3);
    auto* cols = new QSpinBox(&dlg); cols->setRange(1, 20); cols->setValue(3);
    auto* ok = new QPushButton(tr("确定"), &dlg);
    auto* lay = new QVBoxLayout(&dlg);
    auto* r1 = new QHBoxLayout; r1->addWidget(new QLabel(tr("行数:"), &dlg)); r1->addWidget(rows);
    auto* r2 = new QHBoxLayout; r2->addWidget(new QLabel(tr("列数:"), &dlg)); r2->addWidget(cols);
    lay->addLayout(r1); lay->addLayout(r2); lay->addWidget(ok);
    connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted) return;
    size_t curPara = view_->cursorPara();
    if (editor_->insertTableAfter(curPara, (uint32_t)rows->value(), (uint32_t)cols->value())) {
        view_->refreshAfterEdit();
        logPanel_->log(QStringLiteral("INFO"),
                       tr("插入表格：%1 行 × %2 列").arg(rows->value()).arg(cols->value()));
    }
}

void MainWindow::onSave() {
    if (!doc_) return;
    if (openPath_.isEmpty()) { onSaveAs(); return; }
    writeDocxTo(openPath_);
}

void MainWindow::onSaveAs() {
    if (!doc_) return;
    QString path = QFileDialog::getSaveFileName(this, tr("另存为"), openPath_,
                                                tr("Word 文档 (*.docx)"));
    if (!path.isEmpty()) {
        if (writeDocxTo(path)) {
            openPath_ = path;
            // 切换源包到新路径（后续保存的媒体搬运基準）
            srcZip_ = std::make_unique<ZipReader>();
            srcZip_->open(path.toStdString());
        }
    }
}

} // namespace oc::qt
