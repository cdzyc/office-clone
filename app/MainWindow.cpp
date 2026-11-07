#include "MainWindow.h"
#include "LogPanel.h"
#include "PageView.h"
#include "../core/CommandStack.h"
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/EditCommands.h"
#include "../core/FontCheck.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
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
#include <chrono>
#include <thread>

namespace oc::qt {

ZipReader* MainWindow::s_srcZip = nullptr;

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
    QAction* open = file->addAction(tr("打开(&O)..."));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::onOpenDialog);
    QAction* save = file->addAction(tr("保存(&S)"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &MainWindow::onSave);
    QAction* saveAs = file->addAction(tr("另存为(&A)..."));
    connect(saveAs, &QAction::triggered, this, &MainWindow::onSaveAs);
    file->addSeparator();
    QAction* quit = file->addAction(tr("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("编辑(&E)"));
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
    auto connFormat = [&](QToolButton* b, auto fn) {
        connect(b, &QToolButton::clicked, this, [this, fn, b]() {
            if (!syncingToolbar_ && view_) fn(b->isChecked());
        });
    };
    connFormat(boldBtn_, [this](bool v) { view_->boldSelection(v); });
    connFormat(italicBtn_, [this](bool v) { view_->italicSelection(v); });
    connFormat(underlineBtn_, [this](bool v) { view_->underlineSelection(v); });

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
    view_->setDocument(doc_);
    view_->attachEditor(editor_.get(), stack_.get());
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
    logPanel_->log(QStringLiteral("INFO"), tr("打开成功｜耗时 %1 ms").arg(ms, 0, 'f', 0));

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
    if (!openPath_.isEmpty())
        setWindowTitle(tr("%1* — Office Clone").arg(QFileInfo(openPath_).fileName()));
    status_->setText(tr("已修改（Ctrl+S 保存）｜ %1")
                         .arg(QString::fromStdString(doc_->summary())));
}

bool MainWindow::mediaProvider(const std::string&, const std::string& zipPath,
                               std::vector<uint8_t>& out) {
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
    std::vector<std::string> warnings;
    auto t0 = std::chrono::steady_clock::now();
    DocxWriteResult r = DocxWriter::write(targetPath.toStdString(), *doc_,
                                          &MainWindow::mediaProvider, &warnings);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    s_srcZip = nullptr;
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
