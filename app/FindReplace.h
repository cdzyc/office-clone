// oc::qt::FindReplaceDialog — 查找/替换（Ctrl+F / Ctrl+H）
#pragma once
#include <QDialog>

class QLineEdit;
class QPushButton;

namespace oc::qt {

class PageView;

class FindReplaceDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindReplaceDialog(PageView* view, QWidget* parent = nullptr);
    void showFind();          // Ctrl+F（隐藏替换行）
    void showReplace();       // Ctrl+H（完整）

private slots:
    void onFindNext();
    void onReplaceOne();
    void onReplaceAll();

private:
    PageView* view_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLineEdit* replaceEdit_ = nullptr;
    QWidget* replaceRow_ = nullptr;
};

} // namespace oc::qt
