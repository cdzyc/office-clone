#include "MainWindow.h"
#include "LogPanel.h"
#include "PageView.h"
#include "../core/CommandStack.h"
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/EditCommands.h"
#include "../core/FontCheck.h"
#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStatusBar>
#include <QTextEdit>
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

    auto* mainSplit = new QSplitter(Qt::Horizontal, this);
    mainSplit->addWidget(view_);
    mainSplit->addWidget(rightSplit);
    mainSplit->setStretchFactor(0, 1);
    mainSplit->setStretchFactor(1, 0);
    mainSplit->setSizes({950, 330});
    setCentralWidget(mainSplit);

    status_ = new QLabel(tr("就绪"), this);
    statusBar()->addWidget(status_, 1);

    buildMenu();
    connect(view_, &PageView::edited, this, &MainWindow::onEdited);
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
    s_srcZip = srcZip_.get();
    std::vector<std::string> warnings;
    auto t0 = std::chrono::steady_clock::now();
    DocxWriteResult r = DocxWriter::write(targetPath.toStdString(), *doc_,
                                          &MainWindow::mediaProvider, &warnings);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    s_srcZip = nullptr;
    if (!r.ok) {
        logPanel_->log(QStringLiteral("ERROR"), tr("保存失败：%1").arg(QString::fromStdString(r.error)));
        QMessageBox::warning(this, tr("保存失败"), QString::fromStdString(r.error));
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
