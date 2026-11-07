#include "MainWindow.h"
#include "LogPanel.h"
#include "PageView.h"
#include "../core/DocxReader.h"
#include "../core/FontCheck.h"
#include <QAction>
#include <QDockWidget>
#include <QFileDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <vector>

namespace oc::qt {

using Clock = std::chrono::steady_clock;

MainWindow::MainWindow() {
    setWindowTitle(tr("Office Clone — Word"));
    resize(1200, 800);

    view_ = new PageView(this);
    setCentralWidget(view_);

    // 底部停靠日志面板（测试期诊断：右下角可见，可复制粘贴报错）
    logPanel_ = new LogPanel(this);
    auto* dock = new QDockWidget(tr("运行日志（测试版）"), this);
    dock->setWidget(logPanel_);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->resize(width(), 160);

    status_ = new QLabel(tr("就绪"), this);
    statusBar()->addWidget(status_, 1);

    buildMenu();
    logPanel_->log(QStringLiteral("INFO"), tr("office-clone 测试版启动。打开文档试试，报错请点“复制全部”发给开发者。"));
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
    logPanel_->log(QStringLiteral("INFO"), tr("正在打开：%1").arg(path));
    setEnabled(false);

    // 后台线程解析（标书大文件：主线程绝不做重活）
    std::thread([this, path]() {
        auto t0 = Clock::now();
        auto doc = std::make_shared<oc::Document>();
        DocxReadResult r = DocxReader::read(path.toStdString(), *doc);
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

        QString err = r.ok ? QString() : QString::fromStdString(r.error);
        // 回 UI 线程
        QMetaObject::invokeMethod(this, [this, doc, ms, err, warnings = r.warnings]() {
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
    view_->setDocument(doc);
    QString name = QString::fromStdString(doc->sourcePath);
    int slash = name.lastIndexOf('/'); if (slash < 0) slash = name.lastIndexOf('\\');
    setWindowTitle(tr("%1 — Office Clone").arg(name.mid(slash + 1)));
    status_->setText(tr("%1 ｜ 打开耗时 %2 ms ｜ 媒体 %3 MB 未解码（按需加载）")
                         .arg(QString::fromStdString(doc->summary()))
                         .arg(ms, 0, 'f', 0)
                         .arg(doc->stats.mediaBytes / 1048576.0, 0, 'f', 1));
    logPanel_->log(QStringLiteral("INFO"),
                   tr("打开成功：%1 ｜ 耗时 %2 ms").arg(name).arg(ms, 0, 'f', 0));
    logPanel_->log(QStringLiteral("INFO"), QString::fromStdString(doc->summary()));

    // 解析警告（聚合，避免刷屏）
    std::map<QString, int, std::less<QString>> warnCount;
    for (const auto& w : warnings)
        ++warnCount[QString::fromStdString(w)];
    for (auto it = warnCount.begin(); it != warnCount.end(); ++it)
        logPanel_->log(QStringLiteral("WARN"),
                       tr("解析提示 ×%1：%2").arg(it->second).arg(it->first));

    // 字体检查（缺字体是标书打印变形的隐形杀手）
    auto used = FontCheck::collectUsedFonts(*doc);
    if (!used.empty()) {
        std::set<std::string> installed;
        const auto fams = QFontDatabase::families();
        for (const QString& f : fams) installed.insert(f.toStdString());
        auto issues = FontCheck::check(used, installed);
        if (issues.empty()) {
            logPanel_->log(QStringLiteral("INFO"),
                           tr("字体检查：%1 种字体全部已安装 ✓").arg(used.size()));
        } else {
            for (const auto& i : issues)
                logPanel_->log(QStringLiteral("WARN"),
                    tr("字体缺失：%1%2").arg(QString::fromStdString(i.missing))
                        .arg(i.fallback.empty() ? tr("（无回退建议，打印前建议安装）")
                                                : tr("（显示回退为 %1，文档内字体名保持不变）")
                                                      .arg(QString::fromStdString(i.fallback))));
        }
    }
}

} // namespace oc::qt
