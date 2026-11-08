#include "FindReplace.h"
#include "PageView.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace oc::qt {

FindReplaceDialog::FindReplaceDialog(PageView* view, QWidget* parent)
    : QDialog(parent), view_(view) {
    setWindowTitle(tr("查找和替换"));
    setModal(false);
    setMinimumWidth(380);

    findEdit_ = new QLineEdit(this);
    findEdit_->setPlaceholderText(tr("查找内容…"));
    replaceEdit_ = new QLineEdit(this);
    replaceEdit_->setPlaceholderText(tr("替换为…"));

    auto* btnFind = new QPushButton(tr("查找下一个"), this);
    connect(btnFind, &QPushButton::clicked, this, &FindReplaceDialog::onFindNext);
    auto* btnRep = new QPushButton(tr("替换"), this);
    connect(btnRep, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceOne);
    auto* btnAll = new QPushButton(tr("全部替换"), this);
    connect(btnAll, &QPushButton::clicked, this, &FindReplaceDialog::onReplaceAll);

    auto* row1 = new QHBoxLayout;
    row1->addWidget(new QLabel(tr("查找:"), this));
    row1->addWidget(findEdit_, 1);
    row1->addWidget(btnFind);

    auto* row2 = new QHBoxLayout;
    row2->addWidget(new QLabel(tr("替换:"), this));
    row2->addWidget(replaceEdit_, 1);
    row2->addWidget(btnRep);
    row2->addWidget(btnAll);
    replaceRow_ = new QWidget(this);
    replaceRow_->setLayout(row2);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(row1);
    lay->addWidget(replaceRow_);
    connect(findEdit_, &QLineEdit::returnPressed, this, &FindReplaceDialog::onFindNext);
}

void FindReplaceDialog::showFind() {
    replaceRow_->hide();
    show();
    raise();
    activateWindow();
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void FindReplaceDialog::showReplace() {
    replaceRow_->show();
    show();
    raise();
    activateWindow();
    findEdit_->setFocus();
    findEdit_->selectAll();
}

void FindReplaceDialog::onFindNext() {
    if (view_->findNext(findEdit_->text()))
        return;
    QMessageBox::information(this, tr("查找"),
                              tr("未找到“%1”（或已到文档末尾）。").arg(findEdit_->text()));
}

void FindReplaceDialog::onReplaceOne() {
    // 查找命中选区 → 直接替换当前选中
    if (view_->findNext(findEdit_->text())) {
        view_->cutSelection();                       // 删选中（进剪贴板）
        view_->pasteText(replaceEdit_->text());      // 插入替换文本
    }
}

void FindReplaceDialog::onReplaceAll() {
    int n = view_->replaceAll(findEdit_->text(), replaceEdit_->text());
    QMessageBox::information(this, tr("全部替换"),
                             n ? tr("完成：共替换 %1 处。").arg(n)
                               : tr("未找到“%1”。").arg(findEdit_->text()));
}

} // namespace oc::qt
