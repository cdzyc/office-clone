#pragma once
#include "../core/Document.h"
#include "../core/ZipReader.h"
#include <QMainWindow>
#include <memory>

class QLabel;

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

private:
    void buildMenu();
    bool writeDocxTo(const QString& targetPath);

    // 保存时的媒体搬运（从源 docx 长驻句柄读——函数指针桥）
    static bool mediaProvider(const std::string& relId, const std::string& zipPath,
                              std::vector<uint8_t>& out);
    static ZipReader* s_srcZip;               // 保存期间有效

    PageView* view_ = nullptr;
    LogPanel* logPanel_ = nullptr;            // 右下角日志框
    QLabel* infoCard_ = nullptr;              // 右上：文档速览
    QLabel* status_ = nullptr;

    std::shared_ptr<oc::Document> doc_;       // 可编辑文档
    std::unique_ptr<oc::CommandStack> stack_;
    std::unique_ptr<oc::DocumentEditor> editor_;
    std::unique_ptr<ZipReader> srcZip_;       // 源包长驻（保存搬运媒体）
    QString openPath_;
    bool dirty_ = false;
};

} // namespace oc::qt
