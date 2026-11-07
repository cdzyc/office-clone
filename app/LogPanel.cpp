#include "LogPanel.h"
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTime>
#include <QVBoxLayout>

namespace oc::qt {

LogPanel::LogPanel(QWidget* parent) : QWidget(parent) {
    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setMaximumBlockCount(5000);              // 防超大日志内存
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(9);
    view_->setFont(mono);

    auto* btnCopy = new QPushButton(tr("复制全部"), this);
    connect(btnCopy, &QPushButton::clicked, this, &LogPanel::copyAll);
    auto* btnClear = new QPushButton(tr("清空"), this);
    connect(btnClear, &QPushButton::clicked, this, &LogPanel::clearLog);

    auto* btns = new QVBoxLayout;
    btns->addWidget(btnCopy);
    btns->addWidget(btnClear);
    btns->addStretch(1);

    auto* lay = new QHBoxLayout(this);
    lay->addWidget(view_, 1);
    lay->addLayout(btns);
    setLayout(lay);
}

void LogPanel::log(const QString& level, const QString& msg) {
    QString line = QStringLiteral("[%1] [%2] %3")
                       .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                            level.leftJustified(5), msg);
    view_->appendPlainText(line);
    // 错误额外高亮提示（前景色变红）
    if (level == QLatin1String("ERROR")) {
        QTextCursor cur = view_->textCursor();
        cur.movePosition(QTextCursor::End);
        cur.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        fmt.setForeground(QBrush(QColor(200, 30, 30)));
        cur.setCharFormat(fmt);
    }
}

void LogPanel::copyAll() {
    QGuiApplication::clipboard()->setText(view_->toPlainText());
}

void LogPanel::clearLog() {
    view_->clear();
}

} // namespace oc::qt
