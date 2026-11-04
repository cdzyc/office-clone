// m2_tests — M2 模块测试：Deflate / ZipWriter / DocxWriter round-trip /
//            FontCheck / DocumentEditor（含 undo/redo）
// 运行：先 python3 tests/make_fixtures.py，再 ./m2_tests [dataDir]
#include "../core/Deflate.h"
#include "../core/Inflate.h"
#include "../core/ZipReader.h"
#include "../core/ZipWriter.h"
#include "../core/Document.h"
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/FontCheck.h"
#include "../core/EditCommands.h"
#include "../core/Clipboard.h"
#include "../core/CommandStack.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace oc;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

// 媒体搬运 provider：从源 docx 读媒体
static std::string g_srcPath;
static bool mediaFromSrc(const std::string&, const std::string& zipPath,
                         std::vector<uint8_t>& out) {
    return DocxReader::readMedia(g_srcPath, zipPath, out, nullptr);
}

static void testDeflate() {
    std::mt19937 rng(7);
    std::vector<uint8_t> src;
    for (int i = 0; i < 2000; ++i) {
        const char* w = "标书正文内容测试";
        src.insert(src.end(), w, w + strlen(w));
    }
    std::vector<uint8_t> comp, back;
    CHECK(deflate(src, comp), "deflate text ok");
    CHECK(inflate(comp, back), "inflate back ok");
    CHECK(back == src, "deflate roundtrip identical");
    CHECK(comp.size() < src.size() / 4, "deflate text ratio good");

    std::vector<uint8_t> rnd(50000);
    for (auto& b : rnd) b = (uint8_t)(rng() & 0xFF);
    std::vector<uint8_t> c2, b2;
    CHECK(deflate(rnd, c2), "deflate random ok");
    CHECK(inflate(c2, b2) && b2 == rnd, "deflate random identical");

    std::vector<uint8_t> empty, c3, b3;
    CHECK(deflate(empty, c3), "deflate empty ok");
    CHECK(inflate(c3, b3) && b3.empty(), "deflate empty identical");
}

static void testZipWriter(const std::string& dataDir) {
    std::string t1(5000, 'A');
    t1 += "标书内容与office-clone混合数据";
    std::mt19937 rng(3);
    std::vector<uint8_t> raw(30000);
    for (auto& b : raw) b = (uint8_t)(rng() & 0xFF);

    std::string zp = dataDir + "/w_test.zip";
    {
        ZipWriter w;
        std::string err;
        CHECK(w.open(zp, &err), "zipwriter open");
        std::vector<uint8_t> d1(t1.begin(), t1.end());
        CHECK(w.add("text/compressed.txt", d1, true, &err), "add compressed");
        CHECK(w.add("raw.bin", raw, true, &err), "add raw");
        CHECK(w.finalize(&err), "zipwriter finalize");
    }
    ZipReader z;
    std::string err;
    CHECK(z.open(zp, &err), ("rewrite open: " + err).c_str());
    CHECK(z.entries().size() == 2, "rewrite entry count");
    const ZipEntry* e1 = z.find("text/compressed.txt");
    const ZipEntry* e2 = z.find("raw.bin");
    CHECK(e1 && e2, "rewrite find");
    std::vector<uint8_t> out;
    CHECK(e1 && z.read(*e1, out), "rewrite read text");
    CHECK(std::string(out.begin(), out.end()) == t1, "rewrite text identical");
    CHECK(e1 && e1->method == 8, "text deflate method");
    CHECK(e2 && z.read(*e2, out), "rewrite read raw");
    CHECK(out == raw, "rewrite raw identical");
    CHECK(e2 && (e2->method == 0 || e2->compSize <= e2->uncompSize + 16), "raw smart method");
}

static void testDocxRoundTrip(const std::string& dataDir, bool edit, const char* outName) {
    g_srcPath = dataDir + "/mini_bid.docx";
    Document doc;
    DocxReadResult rr = DocxReader::read(g_srcPath, doc);
    CHECK(rr.ok, ("read source: " + rr.error).c_str());
    if (!rr.ok) return;

    std::string titleBefore, para2Before;
    size_t countBefore = 0;
    {
        CommandStack cs;
        DocumentEditor ed(doc, cs);
        countBefore = ed.paragraphCount();
        titleBefore = ed.paragraphText(0);
        para2Before = ed.paragraphText(2);
    }

    CommandStack cs;
    DocumentEditor ed(doc, cs);
    if (edit) {
        CHECK(ed.insertText({0, 0, 0}, "【修订版】"), "edit insert");
        CHECK(ed.setBold({0, 0, 0}, {0, 0, 4}, true), "edit bold");
        CHECK(ed.setAlign(0, Align::Center), "edit align");
        CHECK(ed.splitParagraph({2, 0, 6}), "edit split");
    }

    std::string outPath = dataDir + "/" + outName;
    std::vector<std::string> warnings;
    DocxWriteResult wr = DocxWriter::write(outPath, doc, &mediaFromSrc, &warnings);
    CHECK(wr.ok, ("write: " + wr.error).c_str());
    if (!wr.ok) return;

    Document doc2;
    DocxReadResult rr2 = DocxReader::read(outPath, doc2);
    CHECK(rr2.ok, ("reread: " + rr2.error).c_str());
    if (!rr2.ok) return;

    CommandStack cs2;
    DocumentEditor ed2(doc2, cs2);
    if (!edit) {
        CHECK(ed2.paragraphText(0) == titleBefore, "roundtrip title");
        CHECK(ed2.paragraphText(2) == para2Before, "roundtrip para2");
        CHECK(ed2.paragraphCount() == countBefore, "roundtrip para count");
        CHECK(doc2.stats.tables == doc.stats.tables, "roundtrip tables");
        CHECK(doc2.stats.images == doc.stats.images, "roundtrip images");
        CHECK(doc2.mediaIndex.size() == doc.mediaIndex.size(), "roundtrip media count");
        CHECK(doc2.page.pageWidthTw == doc.page.pageWidthTw &&
              doc2.page.marginLeftTw == doc.page.marginLeftTw, "roundtrip page");
        const Paragraph* t = ed2.paragraphAt(0);
        CHECK(t && t->align == Align::Center && t->styleId == "Title", "roundtrip title fmt");
        CHECK(t && !t->runs.empty() && t->runs[0].bold && t->runs[0].fontSizePt == 22.0,
              "roundtrip run fmt");
        CHECK(t && t->runs[0].fontEastAsia == "仿宋_GB2312", "roundtrip fontEastAsia");
        CHECK(t && t->runs[0].fontAscii == "Times New Roman", "roundtrip fontAscii");
    } else {
        CHECK(ed2.paragraphText(0).find("【修订版】") == 0, "persisted insert");
        const Paragraph* t = ed2.paragraphAt(0);
        CHECK(t && t->align == Align::Center, "persisted align");
        CHECK(t && t->runs[0].bold, "persisted bold");
        CHECK(ed2.paragraphCount() == countBefore + 1, "persisted split");
        CHECK(cs.undo() && ed.paragraphCount() == countBefore, "undo split");
        CHECK(cs.redo() && ed.paragraphCount() == countBefore + 1, "redo split");
    }

    // 媒体字节保真
    std::vector<uint8_t> m1, m2v;
    if (DocxReader::readMedia(g_srcPath, "word/media/img1.png", m1) &&
        DocxReader::readMedia(outPath, "word/media/img1.png", m2v))
        CHECK(m1 == m2v, "media byte-identical");
    else
        CHECK(false, "media read after roundtrip");
}

static void testFontCheck(const std::string& dataDir) {
    Document doc;
    CHECK(DocxReader::read(dataDir + "/mini_bid.docx", doc).ok, "fontcheck read");
    auto used = FontCheck::collectUsedFonts(doc);
    CHECK(used.size() == 2, "fontcheck used count");
    bool hasFs = false, hasTnr = false;
    for (auto& f : used) {
        if (f == "仿宋_GB2312") hasFs = true;
        if (f == "Times New Roman") hasTnr = true;
    }
    CHECK(hasFs && hasTnr, "fontcheck used names");

    auto issues = FontCheck::check(used, {"仿宋", "微软雅黑", "SimSun"});
    CHECK(issues.size() == 2, "fontcheck issue count");
    bool fsOk = false, tnrOk = false;
    for (auto& i : issues) {
        if (i.missing == "仿宋_GB2312" && i.fallback == "仿宋") fsOk = true;
        if (i.missing == "Times New Roman" && i.fallback.empty()) tnrOk = true;
    }
    CHECK(fsOk, "fontcheck fallback fs");
    CHECK(tnrOk, "fontcheck no-fallback tnr");
    CHECK(FontCheck::check(used, {"仿宋_GB2312", "times new roman"}).empty(),
          "fontcheck case-insensitive");
}

static void testEditor() {
    Document doc;
    Node p1{Node::Kind::Paragraph, Paragraph{}, Table{}};
    TextRun r1; r1.text = "Hello ";
    TextRun r2; r2.text = "标书"; r2.bold = true;
    p1.paragraph.runs = {r1, r2};
    Node p2{Node::Kind::Paragraph, Paragraph{}, Table{}};
    TextRun r3; r3.text = "World";
    p2.paragraph.runs = {r3};
    Node tbl{Node::Kind::Table, Paragraph{}, Table{}};
    TextRun r4; r4.text = "表格文字";
    TableCell cell; cell.paragraphs.push_back(Paragraph{});
    cell.paragraphs[0].runs = {r4};
    tbl.table.rows.push_back(TableRow{{cell}});
    doc.body = {p1, p2, tbl};

    CommandStack cs;
    DocumentEditor ed(doc, cs);
    CHECK(ed.paragraphCount() == 3, "editor flat count");
    CHECK(ed.paragraphText(0) == "Hello 标书", "editor text 0");
    CHECK(ed.paragraphText(2) == "表格文字", "editor text cell");

    CHECK(ed.insertText({0, 0, 6}, "BIG "), "editor insert");
    CHECK(ed.paragraphText(0) == "Hello BIG 标书", "editor insert result");
    CHECK(cs.undo() && ed.paragraphText(0) == "Hello 标书", "editor insert undo");
    CHECK(cs.redo() && ed.paragraphText(0) == "Hello BIG 标书", "editor insert redo");

    CHECK(ed.deleteRange({0, 0, 6}, {0, 1, 3}), "editor delete");
    CHECK(ed.paragraphText(0) == "Hello 书", "editor delete result");
    CHECK(cs.undo() && ed.paragraphText(0) == "Hello BIG 标书", "editor delete undo");

    size_t before = ed.paragraphCount();
    CHECK(ed.splitParagraph({1, 0, 3}), "editor split");
    CHECK(ed.paragraphCount() == before + 1, "editor split count");
    CHECK(ed.paragraphText(1) == "Wor", "editor split head");
    CHECK(ed.paragraphText(2) == "ld", "editor split tail");
    CHECK(cs.undo() && ed.paragraphCount() == before, "editor split undo count");
    CHECK(cs.redo(), "editor split redo");

    CHECK(ed.setBold({3, 0, 0}, {3, 0, 4}, true), "editor bold cell");
    CHECK(ed.paragraphAt(3) && ed.paragraphAt(3)->runs[0].bold, "editor bold cell ok");

    CHECK(ed.setAlign(0, Align::Right), "editor align");
    CHECK(doc.body[0].paragraph.align == Align::Right, "editor align ok");
    CHECK(cs.undo() && doc.body[0].paragraph.align == Align::Left, "editor align undo");
}

static void testClipboard() {
    // 构造：p0="Hello BIG 标书"(2 runs) p1="World" p2=表格cell "表格文字"
    Document doc;
    Node p1n{Node::Kind::Paragraph, Paragraph{}, Table{}};
    TextRun r1; r1.text = "Hello ";
    TextRun r2; r2.text = "标书内容"; r2.bold = true;
    p1n.paragraph.runs = {r1, r2};
    Node p2n{Node::Kind::Paragraph, Paragraph{}, Table{}};
    TextRun r3; r3.text = "World";
    p2n.paragraph.runs = {r3};
    Node tbl{Node::Kind::Table, Paragraph{}, Table{}};
    TextRun r4; r4.text = "表格文字";
    TableCell cell; cell.paragraphs.resize(1);
    cell.paragraphs[0].runs = {r4};
    TextRun r5; r5.text = "第二段";
    Paragraph cellP2; cellP2.runs = {r5};
    cell.paragraphs.push_back(cellP2);
    tbl.table.rows.push_back(TableRow{{cell}});
    doc.body = {p1n, p2n, tbl};

    CommandStack cs;
    DocumentEditor ed(doc, cs);

    // ---- 1. 同段复制：拷 "llo 标书"（run0[2..6] + run1 全部）----
    auto f1 = ed.copyRange({0, 0, 2}, {0, 1, 12});
    CHECK(f1.paras.size() == 1, "clip same-para size");
    CHECK(f1.plainText() == "llo 标书内容", "clip same-para text");
    CHECK(f1.paras[0].runs.size() == 2, "clip runs kept");
    CHECK(f1.paras[0].runs[1].bold, "clip format kept");

    // ---- 2. 粘贴单段：粘到 p1 "World" 开头 → "llo 标书内容World" ----
    CHECK(ed.paste({1, 0, 0}, f1), "paste single");
    CHECK(ed.paragraphText(1) == "llo 标书内容World", "paste single result");
    CHECK(cs.undo() && ed.paragraphText(1) == "World", "paste single undo");
    CHECK(cs.redo() && ed.paragraphText(1) == "llo 标书内容World", "paste single redo");

    // ---- 3. 跨段复制（含中间段）----
    // 当前 p1 runs = ["llo ", "标书内容"(b), "World"]：to 指向 run2 的 off3
    auto f2 = ed.copyRange({0, 0, 6}, {1, 2, 3});       // "标书内容"(p0尾) + "Wor"(p1尾段头)
    CHECK(f2.paras.size() == 2, "clip cross-para size");
    CHECK(f2.plainText() == "标书内容\nWor", "clip cross-para text");

    // ---- 4. 多段粘贴：粘到 p1 开头 → 3 段结果：["标书内容", "Wor", "llo 标书内容World"] ----
    size_t cnt = ed.paragraphCount();
    CHECK(ed.paste({1, 0, 0}, f2), "paste multi");
    CHECK(ed.paragraphCount() == cnt + 2, "paste multi count");   // 2段片段+尾段 替换 1段
    CHECK(ed.paragraphText(1) == "标书内容", "paste multi seg1");
    CHECK(ed.paragraphText(2) == "Wor", "paste multi seg2");
    CHECK(ed.paragraphText(3) == "llo 标书内容World", "paste multi tail");
    CHECK(cs.undo() && ed.paragraphCount() == cnt, "paste multi undo");
    CHECK(cs.redo(), "paste multi redo");

    // ---- 5. 跨段删除（body）----
    // redo 后 flat: 0="Hello 标书内容" 1="标书内容" 2="Wor" 3="llo 标书内容World"(runs=[llo ,标书内容(b),World])
    // 删 (1,0,0)→(3,2,0)：保留 p1 前缀"" + p3 后缀"World" → 合并段 "World"
    DocPos a{1, 0, 0}, b{3, 2, 0};
    CHECK(ed.deleteRange(a, b), "delete cross-para");
    CHECK(ed.paragraphText(1) == "World", "delete cross result");
    CHECK(cs.undo() && ed.paragraphText(3) == "llo 标书内容World", "delete cross undo");
    CHECK(cs.redo() && ed.paragraphText(1) == "World", "delete cross redo");

    // ---- 6. cell 内跨段删除 ----
    // redo 后 flat: 0="Hello 标书内容" 1="World"(合并) 2=cell"表格文字" 3=cell"第二段"
    // 删 (2,0,0)→(3,0,9)：cell 两段全删 → 合并空段
    size_t beforeCell = ed.paragraphCount();
    DocPos c{2, 0, 0}, d{3, 0, 9};
    CHECK(ed.deleteRange(c, d), "delete cell cross");
    CHECK(ed.paragraphText(2).empty(), "delete cell result");
    CHECK(ed.paragraphCount() == beforeCell - 1, "delete cell count");
    CHECK(cs.undo() && ed.paragraphText(2) == "表格文字" &&
          ed.paragraphText(3) == "第二段", "delete cell undo");

    // ---- 7. fromPlainText 外部粘贴 ----
    auto f3 = ClipboardFragment::fromPlainText("外部文本\n第二行");
    CHECK(f3.paras.size() == 2, "plain frag size");
    CHECK(f3.plainText() == "外部文本\n第二行", "plain frag text");
    CHECK(ed.paste({0, 0, 0}, f3), "paste external");
    CHECK(ed.paragraphText(0) == "外部文本", "paste external head");
    CHECK(ed.paragraphText(1) == "第二行", "paste external tail");
    CHECK(cs.undo() && ed.paragraphText(0) == "Hello 标书内容", "paste external undo");
}

int main(int argc, char** argv) {
    std::string dataDir = argc > 1 ? argv[1] : "data";
    testDeflate();
    testZipWriter(dataDir);
    testDocxRoundTrip(dataDir, false, "roundtrip_plain.docx");
    testDocxRoundTrip(dataDir, true, "roundtrip_edited.docx");
    testFontCheck(dataDir);
    testEditor();
    testClipboard();
    printf("\n=== M2: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
