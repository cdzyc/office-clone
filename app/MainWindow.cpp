#include "MainWindow.h"
#include "PageView.h"
#include "../core/DocxReader.h"
#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <chrono>
#include <thread>

namespace oc::qt {

using Clock = std::chrono::steady_clock;

MainWindow::MainWindow() {
    setWindowTitle(tr("Office Clone — Word"));
    resize(1200, 800);

    view_ = new PageView(this);
    setCentralWidget(view_);

    status_ = new QLabel(tr("就绪"), this);
    statusBar()->addWidget(status_, 1);

    buildMenu();
}

void MainWindow::buildMenu() {
    QMenu* file = menuBar()->addMenu(tr("文件(&F)"));

    QAction* open = file->addAction(tr("打开(&O)..."));
    open->setShortcut(QKeySequence::Open);                 // 兼容 Qt 6.x 全版本
    connect(open, &QAction::triggered, this, &MainWindow::onOpenDialog);

    file->addSeparator();
    QAction* quit = file->addAction(tr("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    // M2.5：编辑（撤销/重做/复制/粘贴）；M3.5：导出 PDF
}

void MainWindow::onOpenDialog() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("打开 Word 文档"), QString(),
        tr("Word 文档 (*.docx);;所有文件 (*)"));
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::openFile(const QString& path) {
    status_->setText(tr("正在打开：%1 …").arg(path));
    setEnabled(false);

    // 后台线程解析（标书大文件：主线程绝不做重活）
    std::thread([this, path]() {
        auto t0 = Clock::now();
        auto doc = std::make_shared<oc::Document>();
        DocxReadResult r = DocxReader::read(path.toStdString(), *doc);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        QString err = r.ok ? QString() : QString::fromStdString(r.error);
        // 回 UI 线程
        QMetaObject::invokeMethod(this, [this, doc, ms, err]() {
            onLoaded(doc, ms, err);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::onLoaded(std::shared_ptr<oc::Document> doc, double ms, const QString& err) {
    setEnabled(true);
    if (!err.isEmpty()) {
        status_->setText(tr("打开失败：%1").arg(err));
        return;
    }
    view_->setDocument(doc);
    QString name = QString::fromStdString(doc->sourcePath);
    int slash = name.lastIndexOf('/'); if (slash < 0) slash = name.lastIndexOf('\\');
    setWindowTitle(tr("%1 — Office Clone").arg(name.mid(slash + 1)));
    status_->setText(tr("%1 ｜ 打开耗时 %2 ms ｜ 媒体 %3 MB 未解码（按需加载）")
                         .arg(QString::fromStdString(doc->summary()))
                         .arg(ms, 0, 'f', 0)
                         .arg(doc->stats.mediaBytes / 1048576.0, 0, 'f', 1));
}

} // namespace oc::qt
