// oc::qt::LogPanel — 运行日志面板（测试期诊断：错误/警告/耗时可视化 + 一键复制）
// 停靠于主窗口底部（右下角可见），只读 + 时间戳 + 级别标签。
#pragma once
#include <QWidget>

class QPlainTextEdit;

namespace oc::qt {

class LogPanel : public QWidget {
    Q_OBJECT
public:
    explicit LogPanel(QWidget* parent = nullptr);

    // level: INFO / WARN / ERROR（UI 线程调用）
    void log(const QString& level, const QString& msg);

private slots:
    void copyAll();
    void clearLog();

private:
    QPlainTextEdit* view_ = nullptr;
};

} // namespace oc::qt
