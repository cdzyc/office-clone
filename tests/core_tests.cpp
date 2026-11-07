// core_tests — 端到端测试：Inflate / ZipReader / XmlPull / DocxReader / CommandStack
// 运行：先 python3 tests/make_fixtures.py，再 ./core_tests
#include "../core/Inflate.h"
#include "../core/ZipReader.h"
#include "../core/XmlPull.h"
#include "../core/Document.h"
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/CommandStack.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace oc;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static std::vector<uint8_t> loadFile(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v((size_t)n);
    size_t got = fread(v.data(), 1, (size_t)n, f);
    fclose(f);
    v.resize(got);
    return v;
}

static void testInflate(const std::string& dataDir) {
    auto comp = loadFile(dataDir + "/deflate_sample.deflate");
    auto expect = loadFile(dataDir + "/deflate_sample.expected");
    CHECK(!comp.empty() && !expect.empty(), "inflate fixtures exist");
    std::vector<uint8_t> out;
    CHECK(inflate(comp, out), "inflate ok");
    CHECK(out.size() == expect.size(), "inflate size match");
    CHECK(out == expect, "inflate bytes identical");
    // 空流
    std::vector<uint8_t> empty;
    CHECK(!inflate(empty, out), "inflate rejects empty");
}

static void testZip(const std::string& dataDir) {
    ZipReader z;
    std::string err;
    CHECK(z.open(dataDir + "/plain.zip", &err), ("zip open: " + err).c_str());
    CHECK(z.entries().size() == 2, "zip entry count");
    const ZipEntry* st = z.find("stored.txt");
    const ZipEntry* df = z.find("deflated.txt");
    CHECK(st && df, "zip find entries");
    std::vector<uint8_t> out;
    CHECK(st && z.read(*st, out), "stored read");
    CHECK(out.size() > 0 && memcmp(out.data(), "STORED:", 7) == 0, "stored content head");
    CHECK(st && z.crcCheck(out) == st->crc32, "stored crc");
    std::string storedAll(reinterpret_cast<char*>(out.data()), out.size());
    CHECK(storedAll.find("bid document sample") != std::string::npos, "stored content body");
    CHECK(df && z.read(*df, out), "deflated read");
    std::string s2(reinterpret_cast<char*>(out.data()), out.size());
    CHECK(s2.find("DEFLATED: 中文内容测试") == 0, "deflated utf8 content");
    CHECK(s2.find("entity chars: <>&\"'") != std::string::npos, "deflated full content");
    CHECK(df && z.crcCheck(out) == df->crc32, "deflated crc");
    // 不存在条目
    CHECK(z.find("nope.txt") == nullptr, "zip find miss");
}

static void testXml() {
    XmlPullParser p;
    std::string xml = "<?xml version='1.0'?><w:p attr=\"a&amp;b\"><w:t>中文&lt;test&gt;&#x4e2d;</w:t>"
                      "<!-- comment --><w:br/></w:p>";
    p.setInput(xml);
    CHECK(p.next() == XmlEvent::Decl, "xml decl");
    CHECK(p.next() == XmlEvent::StartTag && p.name() == "w:p", "xml root");
    const std::string* a = p.attr("attr");
    CHECK(a && *a == "a&b", "xml attr entity");
    CHECK(p.next() == XmlEvent::StartTag && p.name() == "w:t", "xml w:t start");
    CHECK(p.next() == XmlEvent::Text && p.text() == "中文<test>中", "xml text entities");
    CHECK(p.next() == XmlEvent::EndTag && p.name() == "w:t", "xml w:t end");
    CHECK(p.next() == XmlEvent::Decl, "xml comment skipped");
    CHECK(p.next() == XmlEvent::StartTag && p.name() == "w:br" && p.isSelfClosing(), "xml selfclose");
    CHECK(p.next() == XmlEvent::EndTag && p.name() == "w:br", "xml selfclose end evt");
    CHECK(p.next() == XmlEvent::EndTag && p.name() == "w:p", "xml root end");
    CHECK(p.next() == XmlEvent::None, "xml eof");
    CHECK(p.error().empty(), "xml no error");

    // CDATA
    p.setInput("<a><![CDATA[raw <>& text]]></a>");
    CHECK(p.next() == XmlEvent::StartTag && p.name() == "a", "cdata root");
    CHECK(p.next() == XmlEvent::CData && p.text() == "raw <>& text", "cdata content");
    CHECK(p.next() == XmlEvent::EndTag, "cdata end");
}

static void testDocx(const std::string& dataDir) {
    std::string path = dataDir + "/mini_bid.docx";
    Document doc;
    DocxReadResult r = DocxReader::read(path, doc);
    CHECK(r.ok, ("docx read: " + r.error).c_str());
    if (!r.ok) { printf("  error: %s\n", r.error.c_str()); return; }

    // 统计：正文段落 = 标题1 + 章标题 + 概述段 + 图片段 = 4；表格 1（内含 6 个单元格段）
    // 表格：2 行（3 格 + 2 格）每格 1 段 = 5 段 → 总段落数 4 + 5 = 9
    CHECK(doc.stats.tables == 1, "docx table count");
    CHECK(doc.stats.images == 1, "docx inline image count");
    CHECK(doc.stats.paragraphs == 9, "docx paragraph count");
    CHECK(doc.mediaIndex.size() == 2, "docx media index size");
    CHECK(doc.stats.mediaBytes > 2048, "docx media bytes");   // img2=2048 + img1(真PNG)

    // 标题段落：居中 + 粗体 + 22pt + 文本（含实体）
    const Paragraph* title = nullptr;
    const Table* tbl = nullptr;
    for (const auto& n : doc.body) {
        if (n.kind == Node::Kind::Paragraph) {
            if (!title && !n.paragraph.runs.empty() &&
                n.paragraph.runs[0].text.find("投标文件") != std::string::npos)
                title = &n.paragraph;
        } else if (n.kind == Node::Kind::Table) {
            tbl = &n.table;
        }
    }
    CHECK(title, "docx title found");
    CHECK(title && title->align == Align::Center, "docx title center");
    CHECK(title && title->runs[0].bold, "docx title bold");
    CHECK(title && title->runs[0].fontSizePt == 22.0, "docx title 22pt");
    CHECK(title && title->runs[0].text == "XX项目投标文件 & 技术方案", "docx title text entity");
    CHECK(title && title->styleId == "Title", "docx title style");

    // 斜体 run
    bool foundItalic = false, foundAngleText = false;
    for (const auto& n : doc.body)
        if (n.kind == Node::Kind::Paragraph)
            for (const auto& run : n.paragraph.runs) {
                if (run.italic && run.text == "本项目旨在") foundItalic = true;
                if (run.text.find("中文 <标书> 场景") != std::string::npos) foundAngleText = true;
            }
    CHECK(foundItalic, "docx italic run");
    CHECK(foundAngleText, "docx angle-entity text");

    // 表格：2 行；第一行 3 列；第二行 2 列（gridSpan=2）
    CHECK(tbl, "docx table found");
    CHECK(tbl && tbl->rows.size() == 2, "docx table rows");
    CHECK(tbl && tbl->rows[0].cells.size() == 3, "docx row1 cells");
    CHECK(tbl && tbl->rows[1].cells.size() == 2, "docx row2 cells (span)");
    CHECK(tbl && tbl->rows[1].cells[0].gridSpan == 2, "docx gridSpan");
    CHECK(tbl && tbl->rows[1].cells[1].paragraphs[0].runs[0].bold, "docx cell bold");
    CHECK(tbl && tbl->rows[1].cells[1].paragraphs[0].runs[0].text == "42", "docx cell text");
    CHECK(tbl && tbl->colWidthsDxa.size() == 3 && tbl->colWidthsDxa[0] == 4000, "docx gridCol");

    // 图片：尺寸 EMU
    bool imgOk = false;
    for (const auto& n : doc.body)
        if (n.kind == Node::Kind::Paragraph && !n.paragraph.images.empty()) {
            const auto& im = n.paragraph.images[0];
            imgOk = im.relId == "rId8" && im.cxEmu == 1905000 && im.cyEmu == 1270000;
        }
    CHECK(imgOk, "docx image extent");

    // 页面 A4
    CHECK(doc.page.pageWidthTw == 11906 && doc.page.pageHeightTw == 16838, "docx A4");
    CHECK(doc.page.marginLeftTw == 1800 && doc.page.headerTw == 851, "docx margins");

    // 媒体按需读取（含 CRC）
    std::vector<uint8_t> media;
    std::string merr;
    CHECK(DocxReader::readMedia(path, "word/media/img1.png", media, &merr),
          ("readMedia: " + merr).c_str());
    CHECK(media.size() > 16 && media[0] == 0x89 && media[1] == 0x50, "media is real png");
    CHECK(!DocxReader::readMedia(path, "word/media/nope.png", media, nullptr), "readMedia miss");

    printf("  summary: %s\n", doc.summary().c_str());
}

static void testCommandStack() {
    CommandStack cs;
    int v = 0;
    cs.execute(CommandStack::makeCmd("inc", [&] { v--; }, [&] { v++; }));
    CHECK(v == 1, "cmd exec");
    cs.execute(CommandStack::makeCmd("inc2", [&] { v -= 2; }, [&] { v += 2; }));
    CHECK(v == 3 && cs.canUndo() && cs.isDirty(), "cmd 2nd");
    CHECK(cs.undo(), "undo ok");
    CHECK(v == 1, "undo value");
    CHECK(cs.redo(), "redo ok");
    CHECK(v == 3, "redo value");
    cs.markSaved();
    CHECK(!cs.isDirty(), "saved clean");
    CHECK(cs.revision() == 4, "revision count");
}

static void testChinesePath(const std::string& dataDir) {
    // 真机战报 #1：Windows 下中文文件名打开失败（fopen ANSI 代码页 vs UTF-8 路径）
    // 回归：中文目录 + 中文文件名的读 + 写 round-trip 必须成功
    namespace fs = std::filesystem;
    try {
        fs::path dir = fs::path(dataDir) / "中文目录_测试";
        fs::create_directories(dir);
        fs::path dst = dir / "标书 文档（修复验证）.docx";
        fs::path src = fs::path(dataDir) / "mini_bid.docx";
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);

        // 读
        Document doc;
        DocxReadResult r = DocxReader::read(dst.string(), doc);
        CHECK(r.ok, ("chinese path read: " + r.error).c_str());
        CHECK(r.ok && doc.stats.paragraphs == 9, "chinese path content ok");

        // 写回（写路径同样要求中文安全）
        DocxWriteResult w = DocxWriter::write((dir / "写回_输出.docx").string(), doc);
        CHECK(w.ok, "chinese path write ok");
        Document doc2;
        CHECK(DocxReader::read((dir / "写回_输出.docx").string(), doc2).ok,
              "chinese path reread ok");
    } catch (const std::exception& e) {
        printf("  fs error: %s\n", e.what());
        CHECK(false, "chinese path fs setup");
    }
}

int main(int argc, char** argv) {
    std::string dataDir = argc > 1 ? argv[1] : "data";
    testInflate(dataDir);
    testZip(dataDir);
    testXml();
    testDocx(dataDir);
    testCommandStack();
    testChinesePath(dataDir);
    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
