// oc::qt::MainWindow — 主窗口：后台加载 docx + PageView 虚拟化显示
#pragma once
#include "../core/Document.h"
#include <QMainWindow>
#include <memory>

class QLabel;
class QStatusBar;

namespace oc::qt {

class PageView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

public slots:
    void openFile(const QString& path);

private slots:
    void onOpenDialog();
    void onLoaded(std::shared_ptr<oc::Document> doc, double ms, const QString& err);

private:
    void buildMenu();

    PageView* view_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace oc::qt
