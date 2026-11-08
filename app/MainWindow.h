#pragma once
#include "../core/Document.h"
#include "../core/ImageCache.h"
#include "../core/ZipReader.h"
#include <QMainWindow>
#include <memory>

class QLabel;
class QComboBox;
class QFontComboBox;
class QToolButton;
class QActionGroup;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace oc {
class CommandStack;
class DocumentEditor;
}

namespace oc::qt {

class PageView;
class LogPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow();
    ~MainWindow() override;

public slots:
    void openFile(const QString& path);

private slots:
    void onOpenDialog();
    void onSave();
    void onSaveAs();
    void onLoaded(std::shared_ptr<oc::Document> doc, double ms, const QString& err,
                  const std::vector<std::string>& warnings);
    void onEdited();
    void syncToolbar();                  // 光标移动后同步工具栏状态

private:
    void buildMenu();
    void buildToolbar();
    bool writeDocxTo(const QString& targetPath);

    // 保存时的媒体搬运（从源 docx 长驻句柄读——函数指针桥）
    static bool mediaProvider(const std::string& relId, const std::string& zipPath,
                              std::vector<uint8_t>& out);
    static ZipReader* s_srcZip;               // 保存期间有效

    PageView* view_ = nullptr;
    LogPanel* logPanel_ = nullptr;            // 右下角日志框
    QLabel* infoCard_ = nullptr;              // 右上：文档速览
    QLabel* status_ = nullptr;

    // 工具栏控件
    QComboBox* styleCombo_ = nullptr;
    QFontComboBox* fontCombo_ = nullptr;
    QComboBox* sizeCombo_ = nullptr;
    QToolButton* boldBtn_ = nullptr;
    QToolButton* italicBtn_ = nullptr;
    QToolButton* underlineBtn_ = nullptr;
    QToolButton* strikeBtn_ = nullptr;
    QComboBox* colorCombo_ = nullptr;
    QComboBox* hlCombo_ = nullptr;
    QActionGroup* alignGroup_ = nullptr;
    QList<QAction*> alignActs_;
    bool syncingToolbar_ = false;
    QTreeWidget* outlineTree_ = nullptr;      // 左侧大纲导航
    QTimer* autosaveTimer_ = nullptr;         // 自动保存（静默30s）
    class FindReplaceDialog* findDlg_ = nullptr;
    std::unique_ptr<oc::ImageCache> imageCache_;   // 图片按需解码+LRU
    void rebuildOutline();                    // 扫描 Title/Heading 构建导航树

    std::shared_ptr<oc::Document> doc_;       // 可编辑文档
    std::unique_ptr<oc::CommandStack> stack_;
    std::unique_ptr<oc::DocumentEditor> editor_;
    std::unique_ptr<ZipReader> srcZip_;       // 源包长驻（保存搬运媒体）
    QString openPath_;
    bool dirty_ = false;
};

} // namespace oc::qt
