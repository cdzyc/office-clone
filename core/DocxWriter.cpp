#include "DocxWriter.h"
#include "ZipWriter.h"
#include "PlatformIO.h"
#include "DocxReader.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <set>

namespace oc {
namespace {

// 数字转字符串（避免依赖 std::to_string 的 locale 行为差异）
std::string num(uint64_t v) { return std::to_string(v); }

} // namespace

std::string DocxWriter::xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string DocxWriter::buildDocumentXml(const Document& doc) {
    std::string x;
    x.reserve(4096 + doc.stats.textChars);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    x += "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"";
    x += " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"";
    x += " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\"";
    x += " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><w:body>";

    auto paraXml = [](const Paragraph& p) {
        std::string s = "<w:p>";
        // pPr
        bool hasPpr = !p.styleId.empty() || p.align != Align::Left ||
                      p.spacingBeforePt16 || p.spacingAfterPt16 || p.lineSpacing ||
                      p.pageBreakBefore;
        if (hasPpr) {
            s += "<w:pPr>";
            if (p.pageBreakBefore) s += "<w:pageBreakBefore/>";
            if (!p.styleId.empty())
                s += "<w:pStyle w:val=\"" + xmlEscape(p.styleId) + "\"/>";
            if (p.align == Align::Center) s += "<w:jc w:val=\"center\"/>";
            else if (p.align == Align::Right) s += "<w:jc w:val=\"right\"/>";
            else if (p.align == Align::Justify) s += "<w:jc w:val=\"both\"/>";
            if (p.spacingBeforePt16 || p.spacingAfterPt16 || p.lineSpacing) {
                s += "<w:spacing";
                if (p.spacingBeforePt16) s += " w:before=\"" + num(p.spacingBeforePt16) + "\"";
                if (p.spacingAfterPt16) s += " w:after=\"" + num(p.spacingAfterPt16) + "\"";
                if (p.lineSpacing) s += " w:line=\"" + num(p.lineSpacing) + "\"";
                s += "/>";
            }
            if (p.indLeftTw || p.indRightTw || p.indFirstLineTw) {
                s += "<w:ind";
                if (p.indLeftTw) s += " w:left=\"" + num(p.indLeftTw) + "\"";
                if (p.indRightTw) s += " w:right=\"" + num(p.indRightTw) + "\"";
                if (p.indFirstLineTw > 0)
                    s += " w:firstLine=\"" + num((uint64_t)p.indFirstLineTw) + "\"";
                else if (p.indFirstLineTw < 0)
                    s += " w:hanging=\"" + num((uint64_t)(-p.indFirstLineTw)) + "\"";
                s += "/>";
            }
            s += "</w:pPr>";
        }
        // 内联图片（先图后文——Word 常见顺序）
        for (const auto& img : p.images) {
            s += "<w:r><w:drawing>";
            s += "<wp:extent cx=\"" + num(img.cxEmu) + "\" cy=\"" + num(img.cyEmu) + "\"/>";
            s += "<a:blip r:embed=\"" + xmlEscape(img.relId) + "\"/>";
            s += "</w:drawing></w:r>";
        }
        // 文本 runs
        for (const auto& r : p.runs) {
            s += "<w:r>";
            bool hasRpr = r.bold || r.italic || r.underline || r.strike ||
                          r.fontSizePt > 0 || !r.styleId.empty() ||
                          !r.fontAscii.empty() || !r.fontEastAsia.empty() ||
                          !r.color.empty() || !r.highlight.empty();
            if (hasRpr) {
                s += "<w:rPr>";
                if (!r.styleId.empty())
                    s += "<w:rStyle w:val=\"" + xmlEscape(r.styleId) + "\"/>";
                if (r.bold) s += "<w:b/>";
                if (r.italic) s += "<w:i/>";
                if (r.underline) s += "<w:u/>";
                if (r.strike) s += "<w:strike/>";
                if (!r.color.empty())
                    s += "<w:color w:val=\"" + xmlEscape(r.color) + "\"/>";
                if (!r.highlight.empty())
                    s += "<w:highlight w:val=\"" + xmlEscape(r.highlight) + "\"/>";
                if (!r.fontAscii.empty() || !r.fontEastAsia.empty()) {
                    s += "<w:rFonts";
                    if (!r.fontAscii.empty())
                        s += " w:ascii=\"" + xmlEscape(r.fontAscii) + "\"";
                    if (!r.fontEastAsia.empty())
                        s += " w:eastAsia=\"" + xmlEscape(r.fontEastAsia) + "\"";
                    s += "/>";
                }
                if (r.fontSizePt > 0)
                    s += "<w:sz w:val=\"" + num((uint64_t)(r.fontSizePt * 2 + 0.5)) + "\"/>";
                s += "</w:rPr>";
            }
            s += "<w:t xml:space=\"preserve\">" + xmlEscape(r.text) + "</w:t>";
            s += "</w:r>";
        }
        s += "</w:p>";
        return s;
    };

    for (const auto& node : doc.body) {
        if (node.kind == Node::Kind::Paragraph) {
            x += paraXml(node.paragraph);
        } else {
            const Table& t = node.table;
            x += "<w:tbl><w:tblGrid>";
            for (uint32_t w : t.colWidthsDxa)
                x += "<w:gridCol w:w=\"" + num(w) + "\"/>";
            // 列数兜底（模型列宽缺失时用行内最大列数补齐）
            size_t cols = t.colWidthsDxa.size();
            for (const auto& row : t.rows)
                cols = std::max(cols, row.cells.size());
            while (t.colWidthsDxa.size() < cols)
                x += "<w:gridCol w:w=\"2000\"/>";
            x += "</w:tblGrid>";
            for (const auto& row : t.rows) {
                x += "<w:tr>";
                for (const auto& cell : row.cells) {
                    x += cell.gridSpan > 1
                         ? "<w:tc><w:tcPr><w:gridSpan w:val=\"" + num(cell.gridSpan) + "\"/></w:tcPr>"
                         : "<w:tc>";
                    if (cell.paragraphs.empty())
                        x += "<w:p/>";
                    for (const auto& p : cell.paragraphs)
                        x += paraXml(p);
                    x += "</w:tc>";
                }
                x += "</w:tr>";
            }
            x += "</w:tbl>";
        }
    }

    // sectPr（页面设置）
    const PageSetup& pg = doc.page;
    x += "<w:sectPr><w:pgSz w:w=\"" + num(pg.pageWidthTw) + "\" w:h=\"" + num(pg.pageHeightTw) + "\"/>";
    x += "<w:pgMar w:top=\"" + num(pg.marginTopTw) + "\" w:bottom=\"" + num(pg.marginBottomTw)
       + "\" w:left=\"" + num(pg.marginLeftTw) + "\" w:right=\"" + num(pg.marginRightTw)
       + "\" w:header=\"" + num(pg.headerTw) + "\" w:footer=\"" + num(pg.footerTw) + "\"/>";
    x += "</w:sectPr></w:body></w:document>";
    return x;
}

std::string DocxWriter::buildRelsXml(const Document& doc) {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    x += "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (const auto& m : doc.mediaIndex)
        x += "<Relationship Id=\"" + xmlEscape(m.relId)
           + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\""
           + xmlEscape(m.zipPath.substr(m.zipPath.rfind("word/") == 0 ? 5 : 0))
           + "\"/>";
    for (const auto& hf : doc.headerFooter)
        x += "<Relationship Id=\"" + xmlEscape(hf.relId)
           + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
           + hf.kind + "\" Target=\"" + xmlEscape(hf.zipPath.substr(5)) + "\"/>";
    x += "</Relationships>";
    return x;
}

std::string DocxWriter::buildContentTypesXml(const Document& doc) {
    std::set<std::string> exts;
    for (const auto& m : doc.mediaIndex) {
        size_t dot = m.zipPath.rfind('.');
        if (dot != std::string::npos) {
            std::string e = m.zipPath.substr(dot + 1);
            std::transform(e.begin(), e.end(), e.begin(), ::tolower);
            exts.insert(e);
        }
    }
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    x += "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">";
    x += "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>";
    x += "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
    for (const auto& e : exts) {
        std::string ct = "application/octet-stream";
        if (e == "png") ct = "image/png";
        else if (e == "jpg" || e == "jpeg") ct = "image/jpeg";
        else if (e == "gif") ct = "image/gif";
        else if (e == "bmp") ct = "image/bmp";
        else if (e == "emf") ct = "image/x-emf";
        else if (e == "wmf") ct = "image/x-wmf";
        x += "<Default Extension=\"" + e + "\" ContentType=\"" + ct + "\"/>";
    }
    x += "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>";
    for (const auto& hf : doc.headerFooter) {
        std::string ct = hf.kind == "header"
            ? "application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml"
            : "application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml";
        x += "<Override PartName=\"/" + hf.zipPath + "\" ContentType=\"" + ct + "\"/>";
    }
    x += "</Types>";
    return x;
}

std::string DocxWriter::buildRootRelsXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Id=\"rId1\" "
           "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
           "Target=\"word/document.xml\"/>"
           "</Relationships>";
}

DocxWriteResult DocxWriter::write(const std::string& path, const Document& doc,
                                  MediaProvider media, std::vector<std::string>* warnings) {
    DocxWriteResult r;
    ZipWriter zw;
    if (!zw.open(path, &r.error)) return r;

    auto addStr = [&](const char* name, const std::string& s) -> bool {
        std::vector<uint8_t> v(s.begin(), s.end());
        return zw.add(name, v, true, &r.error);
    };

    if (!addStr("[Content_Types].xml", buildContentTypesXml(doc))) return r;
    if (!addStr("_rels/.rels", buildRootRelsXml())) return r;
    if (!addStr("word/document.xml", buildDocumentXml(doc))) return r;
    if (!addStr("word/_rels/document.xml.rels", buildRelsXml(doc))) return r;

    // 媒体搬运（字节保真）
    for (const auto& m : doc.mediaIndex) {
        std::vector<uint8_t> data;
        bool ok = false;
        if (media) ok = media(m.relId, m.zipPath, data);
        if (!ok) {
            if (warnings)
                warnings->push_back("media not carried over: " + m.zipPath);
            continue;
        }
        if (!zw.add(m.zipPath, data, true, &r.error)) return r;
    }
    // 页眉/页脚部件保真搬运（批次G：防保存丢页眉）
    for (const auto& hf : doc.headerFooter) {
        std::vector<uint8_t> data;
        bool ok = media && media(hf.relId, hf.zipPath, data);
        if (ok) {
            if (!zw.add(hf.zipPath, data, true, &r.error)) return r;
        } else if (warnings) {
            warnings->push_back("header/footer not carried over: " + hf.zipPath);
        }
    }

    if (!zw.finalize(&r.error)) return r;

    FILE* f = oc::fopenUtf8(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); r.writtenBytes = (uint64_t)ftell(f); fclose(f); }
    r.ok = true;
    return r;
}

} // namespace oc
