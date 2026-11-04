// oc::Document — Word 文档内存模型（自研，不依赖 Qt）
// 设计原则：
//  1. 值类型 + 紧凑布局（arena 分配 M3 优化）；2. 图片只存引用不解码；
//  3. 模型与渲染解耦：布局引擎消费 Document，渲染层消费布局结果。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oc {

enum class Align : uint8_t { Left, Center, Right, Justify };

struct TextRun {
    std::string text;          // UTF-8
    std::string styleId;       // w:rStyle（字符样式）
    bool bold = false, italic = false, underline = false;
    double fontSizePt = 0;     // 0 = 继承样式/默认
    std::string fontAscii;     // w:rFonts/@w:ascii（西文字体）
    std::string fontEastAsia;  // w:rFonts/@w:eastAsia（中文字体——标书关键）
};

using RunList = std::vector<TextRun>;

struct ImageRun {              // w:drawing(inline pic) —— 首版以图片 Run 形式挂在段落
    std::string relId;         // 指向 mediaIndex
    uint32_t cxEmu = 0, cyEmu = 0;   // EMU（1px = 9525 EMU）
};

struct Paragraph {
    RunList runs;
    std::vector<ImageRun> images;
    std::string styleId;       // w:pStyle（段落样式，Heading1 等）
    Align align = Align::Left;
    uint32_t spacingBeforePt16 = 0, spacingAfterPt16 = 0;  // 1/16 pt（OOXML 原始单位 twentieths）
    uint32_t lineSpacing = 0;                             // 240 = 单倍
};

struct TableCell {
    std::vector<Paragraph> paragraphs;
    uint32_t gridSpan = 1;
};

struct TableRow {
    std::vector<TableCell> cells;
    // trHeight M4
};

struct Table {
    std::vector<TableRow> rows;
    std::vector<uint32_t> colWidthsDxa;    // twips
};

// Body 节点：variant 风格
struct Node {
    enum class Kind { Paragraph, Table } kind = Kind::Paragraph;
    Paragraph paragraph;
    Table table;
};

struct MediaEntry {
    std::string relId;
    std::string zipPath;       // word/media/xxx.png
    std::string contentType;
    uint64_t byteSize = 0;     // 压缩包内条目大小（索引信息——不解码）
};

struct PageSetup {             // w:sectPr（首版取第一个 sectPr 的页面级设置）
    uint32_t pageWidthTw = 11906, pageHeightTw = 16838;   // A4 默认（twips）
    uint32_t marginTopTw = 1440, marginBottomTw = 1440;
    uint32_t marginLeftTw = 1800, marginRightTw = 1800;
    uint32_t headerTw = 851, footerTw = 992;
};

struct DocumentStats {
    uint32_t paragraphs = 0, tables = 0, images = 0;
    uint64_t mediaBytes = 0;
    uint32_t textChars = 0;    // UTF-8 字节近似
};

class Document {
public:
    std::vector<Node> body;
    std::vector<MediaEntry> mediaIndex;   // relId → zip 路径（按需解码的索引）
    PageSetup page;
    DocumentStats stats;
    std::string sourcePath;               // 打开的 docx 路径

    // 序列化摘要（调试/测试/状态栏）
    std::string summary() const;
};

} // namespace oc
