// office-clone — Qt 6 应用入口
// 本地构建：cmake -DOC_BUILD_QT=ON ..（需要 Qt 6.4+ / MSVC 或 MinGW）
#include "MainWindow.h"
#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("office-clone");
    app.setApplicationName("OfficeClone");

    oc::qt::MainWindow w;
    w.show();

    // 命令行直接打开文档
    if (argc > 1)
        w.openFile(QString::fromLocal8Bit(argv[1]));

    return app.exec();
}
