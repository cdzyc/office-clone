// oc::DocumentEditor — 模型层编辑命令（M2）
// 设计：
//  1. 扁平段落视图：全文档段落线性编号（body 段落 + 表格单元格内段落统一编址）；
//  2. 所有编辑走 CommandStack（撤销/重做免费获得）；
//  3. DocPos.off 为 UTF-8 字节偏移，调用方（UI）保证字符边界。
#pragma once
#include "Clipboard.h"
#include "CommandStack.h"
#include "Document.h"
#include <functional>

namespace oc {

struct DocPos {
    size_t para = 0;   // 扁平段落索引
    size_t run = 0;    // 段内 run 索引
    size_t off = 0;    // run 内 UTF-8 字节偏移
    bool operator==(const DocPos& o) const {
        return para == o.para && run == o.run && off == o.off;
    }
    bool operator<(const DocPos& o) const {
        if (para != o.para) return para < o.para;
        if (run != o.run) return run < o.run;
        return off < o.off;
    }
};

class DocumentEditor {
public:
    DocumentEditor(Document& doc, CommandStack& stack) : doc_(doc), stack_(stack) {}

    // ---- 扁平段落视图 ----
    size_t paragraphCount() const;
    Paragraph* paragraphAt(size_t flat);                 // 越界返回 nullptr
    const Paragraph* paragraphAt(size_t flat) const;

    // ---- 编辑命令（全部可撤销）----
    // 在 pos 处插入文本（继承所在 run 格式；空段则新建 run）
    bool insertText(DocPos pos, const std::string& utf8);
    // 删除 [from, to)。支持同段与跨段（同 body 区段或同 cell 内；跨结构拒绝）
    bool deleteRange(DocPos from, DocPos to);
    // 在 pos 处拆分段落（回车）。body 段落后插新节点；单元格内在 cell 内追加。
    bool splitParagraph(DocPos pos);
    // 同段范围设置粗体
    bool setBold(DocPos from, DocPos to, bool val);
    // 同段范围：斜体/下划线/字号(pt)/字体(eastAsia+ascii)——字符级（自动拆分 runs）
    bool setItalic(DocPos from, DocPos to, bool val);
    bool setUnderline(DocPos from, DocPos to, bool val);
    bool setFontSize(DocPos from, DocPos to, double pt);
    bool setFontName(DocPos from, DocPos to, const std::string& eastAsia,
                     const std::string& ascii);
    bool setStrike(DocPos from, DocPos to, bool val);
    bool setTextColor(DocPos from, DocPos to, const std::string& hexRgb);
    bool setHighlight(DocPos from, DocPos to, const std::string& colorName);
    // 段落样式（正文/标题1…）与对齐
    bool setParaStyle(size_t flatPara, const std::string& styleId);
    bool setAlign(size_t flatPara, Align a);
    // 段落级 runs 整体替换（查找替换/导入重构用；命令化可撤销）
    void replaceRunsCommand(size_t flatPara, RunList oldRuns, RunList newRuns,
                            const std::string& name);
    // 全文档查找替换（大小写不敏感，保留段首格式；返回替换数；逐段命令可撤销）
    int replaceAll(const std::string& from, const std::string& to);
    // 段落缩进（twips；firstLine 负=悬挂）与行距（240=单倍）
    bool setIndent(size_t flatPara, uint32_t left, uint32_t right, int firstLine);
    bool setLineSpacing(size_t flatPara, uint32_t line240);
    // 批次C：在段落插入内联图片（可撤销；媒体条目由调用方维护 Document::mediaIndex）
    bool insertImage(size_t flatPara, const std::string& relId,
                     uint32_t cxEmu, uint32_t cyEmu);
    // 在指定段之后插入 rows×cols 空表格（列宽均分 A4 正文宽）
    bool insertTableAfter(size_t flatPara, uint32_t rows, uint32_t cols);
    // 批次G：分页符（段前分页）与图片删除
    bool setPageBreakBefore(size_t flatPara, bool val);
    bool removeImage(size_t flatPara, size_t imageIdx);
    // 批次G：表格行列操作（作用于光标所在表；body 级快照撤销）
    bool tableInsertRowAfter(size_t flatPara);
    bool tableDeleteRow(size_t flatPara);
    bool tableInsertColRight(size_t flatPara);
    bool tableDeleteCol(size_t flatPara);
    // 批次G：在文档开头插入多个段落（目录生成）
    bool insertParagraphsAtStart(const std::vector<Paragraph>& paras);
    // 图片列表整体替换（UI 拖拽缩放用）
    void replaceRunsCommand2(size_t flatPara, std::vector<ImageRun> oldImgs,
                             std::vector<ImageRun> newImgs);

    // ---- 剪贴板（M2.6）----
    // 复制 [from, to) 为富文本片段（跨段支持，保留 run 格式与段落结构）
    ClipboardFragment copyRange(DocPos from, DocPos to);
    // 粘贴：单段片段在 pos 处插入 runs；多段片段拆当前段并插入（仅 body 段落目标）
    bool paste(DocPos pos, const ClipboardFragment& frag);

    // ---- 便捷 ----
    std::string paragraphText(size_t flat) const;        // 调试/测试
    static DocPos endOf(size_t flatPara, const Document& doc);// 段落末尾位置

private:
    // 字符级格式通用实现（选区∩run 求交 + 相邻同格式合并；命令化可撤销）
    bool formatRange(DocPos from, DocPos to,
                     const std::function<void(TextRun&)>& mutate, const char* name);
    struct ParaLoc {
        Node* node = nullptr;
        Paragraph* p = nullptr;
        size_t bodyIdx = 0;
        size_t cellIdx = 0;                              // SIZE_MAX = body 段落
        size_t paraInCell = 0;
    };
    ParaLoc locate(size_t flat);                         // 越界返回 {nullptr,...}
    Document& doc_;
    CommandStack& stack_;
};

} // namespace oc
