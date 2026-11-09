#include "DocxReader.h"
#include "XmlPull.h"
#include <algorithm>
#include <cstdlib>

namespace oc {
namespace {

uint32_t parseU(const std::string* s, uint32_t dflt = 0) {
    if (!s || s->empty()) return dflt;
    return (uint32_t)std::strtoul(s->c_str(), nullptr, 10);
}

// OOXML twips → pt 用 /20；这里保留 twips 原始值（渲染层换算）

} // namespace

DocxReadResult DocxReader::read(const std::string& path, Document& doc) {
    DocxReadResult r;
    ZipReader zip;
    if (!zip.open(path, &r.error)) return r;

    doc = Document{};
    doc.sourcePath = path;

    // 1) 关系文件：word/_rels/document.xml.rels → 媒体索引
    if (const ZipEntry* rels = zip.find("word/_rels/document.xml.rels")) {
        std::vector<uint8_t> data;
        if (zip.read(*rels, data) && !parseRels(data, "word/", doc, &r.headerFooterParts)) {
            r.error = "parse document.xml.rels failed";
            return r;
        }
        // 媒体索引补全：从 zip 条目取真实大小（索引信息——不解码）
        for (auto& m : doc.mediaIndex)
            if (const ZipEntry* e = zip.find(m.zipPath))
                m.byteSize = e->uncompSize;
    } else {
        r.warnings.push_back("word/_rels/document.xml.rels missing (no media index)");
    }

    // 2) 主文档（绝大多数标书是标准路径；非标准的用 [Content_Types] 定位 M2 补）
    const ZipEntry* docXml = zip.find("word/document.xml");
    if (!docXml) { r.error = "word/document.xml not found"; return r; }
    std::vector<uint8_t> data;
    if (!zip.read(*docXml, data, 256ull * 1024 * 1024, &r.error)) return r;

    if (!parseDocumentXml(data, doc, "word/", r.warnings)) {
        r.error = r.error.empty() ? "parse document.xml failed" : r.error;
        return r;
    }

    // 统计
    doc.stats.tables = 0; doc.stats.images = 0; doc.stats.textChars = 0; doc.stats.paragraphs = 0;
    for (const auto& n : doc.body) {
        if (n.kind == Node::Kind::Table) {
            ++doc.stats.tables;
            for (const auto& row : n.table.rows)
                for (const auto& cell : row.cells)
                    for (const auto& p : cell.paragraphs) {
                        ++doc.stats.paragraphs;
                        doc.stats.images += (uint32_t)p.images.size();
                        for (const auto& run : p.runs) doc.stats.textChars += (uint32_t)run.text.size();
                    }
        } else {
            const auto& p = n.paragraph;
            ++doc.stats.paragraphs;
            doc.stats.images += (uint32_t)p.images.size();
            for (const auto& run : p.runs) doc.stats.textChars += (uint32_t)run.text.size();
        }
    }
    doc.stats.mediaBytes = 0;
    for (const auto& m : doc.mediaIndex) doc.stats.mediaBytes += m.byteSize;

    r.ok = true;
    return r;
}

bool DocxReader::readMedia(const std::string& path, const std::string& zipPath,
                           std::vector<uint8_t>& out, std::string* err) {
    ZipReader zip;
    if (!zip.open(path, err)) return false;
    const ZipEntry* e = zip.find(zipPath);
    if (!e) { if (err) *err = "media not found: " + zipPath; return false; }
    if (!zip.read(*e, out, 512ull * 1024 * 1024, err)) return false;
    // CRC 校验（图片损坏早发现）
    uint32_t crc = zip.crcCheck(out);
    if (crc != e->crc32) { if (err) *err = "media crc mismatch: " + zipPath; return false; }
    return true;
}

bool DocxReader::parseRels(const std::vector<uint8_t>& xml, const std::string& base,
                           Document& doc, std::vector<std::string>* hfParts) {
    std::string_view sv(reinterpret_cast<const char*>(xml.data()), xml.size());
    XmlPullParser p;
    p.setInput(sv);
    while (p.next() != XmlEvent::None) {
        if (p.type() == XmlEvent::Error) return false;
        if (p.type() != XmlEvent::StartTag) continue;
        if (p.name() == "Relationship") {
            const std::string* id = p.attr("Id");
            const std::string* target = p.attr("Target");
            const std::string* type = p.attr("Type");
            if (id && target && type && type->find("/image") != std::string::npos) {
                MediaEntry m;
                m.relId = *id;
                m.zipPath = base + *target;
                doc.mediaIndex.push_back(std::move(m));
            } else if (id && target && type &&
                       (type->find("/header") != std::string::npos ||
                        type->find("/footer") != std::string::npos)) {
                Document::HFPart hf;
                hf.relId = *id;
                hf.zipPath = base + *target;
                hf.kind = type->find("/header") != std::string::npos ? "header" : "footer";
                doc.headerFooter.push_back(hf);
            }
        }
    }
    return p.error().empty();
}

// ---- document.xml 状态机 ----
// 简化：不建 DOM，遇到 w:p/w:tbl 直接产出 Node。
bool DocxReader::parseDocumentXml(const std::vector<uint8_t>& xml, Document& doc,
                                  const std::string& mediaBase,
                                  std::vector<std::string>& warnings) {
    std::string_view sv(reinterpret_cast<const char*>(xml.data()), xml.size());
    XmlPullParser p;
    p.setInput(sv);

    // 栈式状态：w:body > w:p | w:tbl > w:tr > w:tc > w:p
    enum class Ctx { None, Body, Para, Run, Tbl, Row, Cell } stack[16];
    int top = -1;
    auto push = [&](Ctx c) { if (top < 15) stack[++top] = c; };
    auto pop  = [&](Ctx c) { if (top >= 0 && stack[top] == c) --top; };

    Paragraph curPara;
    TextRun curRun;
    Table curTable;
    TableCell curCell;
    bool haveRun = false;
    bool inRunPr = false;
    // 图片尺寸缓存（wp:extent 可能先于 a:blip 出现）
    uint32_t pendCx = 0, pendCy = 0;
    bool havePend = false;

    auto flushRun = [&]() {
        if (haveRun) {
            if (!curRun.text.empty()) curPara.runs.push_back(curRun);
            curRun = TextRun{}; haveRun = false;
        }
    };
    auto flushPara = [&]() {
        flushRun();
        doc.body.push_back(Node{Node::Kind::Paragraph, std::move(curPara), Table{}});
        curPara = Paragraph{};
    };
    auto flushCell = [&]() {
        if (curCell.paragraphs.empty()) curCell.paragraphs.push_back(Paragraph{});
        curTable.rows.back().cells.push_back(std::move(curCell));
        curCell = TableCell{};
    };

    bool sawSectPr = false;
    while (true) {
        XmlEvent ev = p.next();
        if (ev == XmlEvent::None) break;
        if (ev == XmlEvent::Error) return false;
        if (ev != XmlEvent::StartTag && ev != XmlEvent::EndTag) continue;

        Ctx cur = top >= 0 ? stack[top] : Ctx::None;
        const std::string& tag = p.name();

        if (ev == XmlEvent::StartTag) {
            // ---- 文本与特殊字符（w:t / w:tab / w:br）----
            if (tag == "w:t" && cur == Ctx::Run) {
                curRun.text += p.readElementText();
                continue;
            }
            if (tag == "w:tab" && cur == Ctx::Run) { curRun.text += "\t"; continue; }
            if (tag == "w:br" && cur == Ctx::Run) {
                curRun.text += "\n";
                continue;
            }

            // ---- 进入新上下文 ----
            if (tag == "w:body") { push(Ctx::Body); continue; }
            if (tag == "w:tbl") {
                // 嵌套表检测：父栈中已有 Tbl → 首版警告
                for (int i = 0; i <= top; ++i)
                    if (stack[i] == Ctx::Tbl) { warnings.push_back("nested table flattened (M4)"); break; }
                curTable = Table{};
                push(Ctx::Tbl);
                continue;
            }
            if (tag == "w:tr" && (cur == Ctx::Tbl || top >= 0)) {
                curTable.rows.emplace_back();
                push(Ctx::Row);
                continue;
            }
            if (tag == "w:tc" && top >= 0) { push(Ctx::Cell); curCell = TableCell{}; continue; }
            if (tag == "w:p" && top >= 0) { push(Ctx::Para); curPara = Paragraph{}; continue; }
            if (tag == "w:r" && top >= 0) { curRun = TextRun{}; haveRun = true; push(Ctx::Run); continue; }

            // ---- 属性读取 ----
            if (tag == "w:rPr" && cur == Ctx::Run) { inRunPr = true; continue; }
            if (tag == "w:b" && inRunPr) { curRun.bold = true; continue; }
            if (tag == "w:i" && inRunPr) { curRun.italic = true; continue; }
            if (tag == "w:u" && inRunPr) { curRun.underline = true; continue; }
            if (tag == "w:strike" && inRunPr) { curRun.strike = true; continue; }
            if (tag == "w:color" && inRunPr) {
                if (const std::string* v = p.attr("w:val")) curRun.color = *v;
                continue;
            }
            if (tag == "w:highlight" && inRunPr) {
                if (const std::string* v = p.attr("w:val")) curRun.highlight = *v;
                continue;
            }
            if (tag == "w:rFonts" && inRunPr) {
                if (const std::string* v = p.attr("w:ascii")) curRun.fontAscii = *v;
                if (const std::string* v = p.attr("w:eastAsia")) curRun.fontEastAsia = *v;
                continue;
            }
            if (tag == "w:sz" && inRunPr) {
                const std::string* v = p.attr("w:val");
                curRun.fontSizePt = parseU(v) / 2.0;   // 半磅
                continue;
            }
            if (tag == "w:pPr" && cur == Ctx::Para) continue;
            if (tag == "w:pStyle") {
                if (const std::string* v = p.attr("w:val")) curPara.styleId = *v;
                continue;
            }
            if (tag == "w:jc") {
                if (const std::string* v = p.attr("w:val")) {
                    if (*v == "center") curPara.align = Align::Center;
                    else if (*v == "right") curPara.align = Align::Right;
                    else if (*v == "both") curPara.align = Align::Justify;
                }
                continue;
            }
            if (tag == "w:spacing" && cur == Ctx::Para) {
                curPara.spacingBeforePt16 = parseU(p.attr("w:before"));
                curPara.spacingAfterPt16 = parseU(p.attr("w:after"));
                curPara.lineSpacing = parseU(p.attr("w:line"), 240);
                continue;
            }
            if (tag == "w:pageBreakBefore" && cur == Ctx::Para) { curPara.pageBreakBefore = true; continue; }
            if (tag == "w:ind") {
                // ⚠️ 真机战报 #5：OOXML 允许负缩进（如 left=-425），
                // uint32 解析会爆成 42 亿 → 渲染行宽为负 → 崩溃。一律 clamp ≥0。
                auto parseUClamped = [](const std::string* s) -> uint32_t {
                    if (!s || s->empty()) return 0;
                    long v = strtol(s->c_str(), nullptr, 10);
                    return v > 0 ? (uint32_t)v : 0;
                };
                curPara.indLeftTw = parseUClamped(p.attr("w:left"));
                curPara.indRightTw = parseUClamped(p.attr("w:right"));
                curPara.indFirstLineTw = 0;
                if (const std::string* fl = p.attr("w:firstLine"))
                    curPara.indFirstLineTw = (int)parseUClamped(fl);
                else if (const std::string* hg = p.attr("w:hanging"))
                    curPara.indFirstLineTw = -(int)parseUClamped(hg);
                continue;
            }
            if (tag == "w:gridSpan") {
                curCell.gridSpan = parseU(p.attr("w:val"), 1);
                continue;
            }
            if (tag == "w:tblGrid") continue;   // 列宽在 w:gridCol（不压栈）
            if (tag == "w:gridCol") {
                curTable.colWidthsDxa.push_back(parseU(p.attr("w:w")));
                continue;
            }
            if (tag == "w:sectPr") {
                if (!sawSectPr) sawSectPr = true;
                else warnings.push_back("multiple sectPr: only first parsed (M4)");
                continue;
            }
            if (tag == "w:pgSz") {
                doc.page.pageWidthTw = parseU(p.attr("w:w"), doc.page.pageWidthTw);
                doc.page.pageHeightTw = parseU(p.attr("w:h"), doc.page.pageHeightTw);
                continue;
            }
            if (tag == "w:pgMar") {
                doc.page.marginTopTw = parseU(p.attr("w:top"), doc.page.marginTopTw);
                doc.page.marginBottomTw = parseU(p.attr("w:bottom"), doc.page.marginBottomTw);
                doc.page.marginLeftTw = parseU(p.attr("w:left"), doc.page.marginLeftTw);
                doc.page.marginRightTw = parseU(p.attr("w:right"), doc.page.marginRightTw);
                doc.page.headerTw = parseU(p.attr("w:header"), doc.page.headerTw);
                doc.page.footerTw = parseU(p.attr("w:footer"), doc.page.footerTw);
                continue;
            }
            if (tag == "w:drawing") {
                warnings.push_back("w:drawing met (image inline parsed only)");
                continue;
            }
            // wp:extent 与 a:blip 的顺序在真实文档中两种都存在——兼容处理
            if (tag == "wp:extent") {
                uint32_t cx = parseU(p.attr("cx")), cy = parseU(p.attr("cy"));
                if (!curPara.images.empty() &&
                    curPara.images.back().cxEmu == 0 && curPara.images.back().cyEmu == 0) {
                    curPara.images.back().cxEmu = cx;      // blip 先出现：补尺寸
                    curPara.images.back().cyEmu = cy;
                } else {
                    pendCx = cx; pendCy = cy; havePend = true;  // extent 先出现：缓存
                }
                continue;
            }
            if (tag == "a:blip") {
                if (const std::string* rid = p.attr("r:embed")) {
                    ImageRun img;
                    img.relId = *rid;
                    if (havePend) { img.cxEmu = pendCx; img.cyEmu = pendCy; havePend = false; }
                    curPara.images.push_back(img);
                }
                continue;
            }
            continue;
        }

        // ---- EndTag ----
        if (tag == "w:r" && cur == Ctx::Run) { flushRun(); pop(Ctx::Run); continue; }
        if (tag == "w:p" && cur == Ctx::Para) {
            if (top >= 1 && stack[top - 1] == Ctx::Cell) {
                flushRun();
                curCell.paragraphs.push_back(std::move(curPara));
                curPara = Paragraph{};
            } else {
                flushPara();
            }
            pop(Ctx::Para);
            continue;
        }
        if (tag == "w:tc" && cur == Ctx::Cell) { flushCell(); pop(Ctx::Cell); continue; }
        if (tag == "w:tr" && cur == Ctx::Row) { pop(Ctx::Row); continue; }
        if (tag == "w:tbl" && cur == Ctx::Tbl) {
            doc.body.push_back(Node{Node::Kind::Table, Paragraph{}, std::move(curTable)});
            curTable = Table{};
            pop(Ctx::Tbl);
            continue;
        }
        if (tag == "w:body" && cur == Ctx::Body) { pop(Ctx::Body); continue; }
        if (tag == "w:rPr") { inRunPr = false; continue; }
    }
    (void)mediaBase;
    return p.error().empty();
}

} // namespace oc
