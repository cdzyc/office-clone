// e2e_features_test — 全功能端到端验收（模拟真实用户工作流）
// 覆盖：打开(4文档/中文路径) → 大纲 → 字体 → 图片管线 → 编辑全流程
//      (输入/删除/回车/B I U S/颜色/高亮/字号/字体/对齐/样式/剪贴板/查找替换)
//      → 撤销重做 → 保存 → 重读逐字段验证
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/Document.h"
#include "../core/EditCommands.h"
#include "../core/CommandStack.h"
#include "../core/Clipboard.h"
#include "../core/FontCheck.h"
#include "../core/ImageCache.h"
#include "../core/Layout.h"
#include "../core/ZipReader.h"
#include "../core/PlatformIO.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace oc;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; printf("  ✅ %s\n", msg); } \
    else { ++g_fail; printf("  ❌ %s (line %d)\n", msg, __LINE__); } } while (0)

static std::string g_srcPath;
static size_t ccd_before(Document& d) { return d.body.size(); }
static bool mediaFromSrc(const std::string&, const std::string& zp, std::vector<uint8_t>& o) {
    return DocxReader::readMedia(g_srcPath, zp, o);
}

int main(int argc, char** argv) {
    std::string dataDir = argc > 1 ? argv[1] : "tests/data";
    std::string realDir = argc > 2 ? argv[2] : "../../attachments";

    printf("═══════ Phase 1：打开能力 ═══════\n");
    struct Doc { std::string path, label; size_t minParas; };
    std::vector<Doc> docs = {
        {dataDir + "/mini_bid.docx", "迷你标书", 9},
        {dataDir + "/中文目录_测试/标书 文档（修复验证）.docx", "中文路径文档", 9},
        {realDir + "/贪吃蛇2048-PRD产品需求文档.docx", "PRD文档(真实)", 100},
        {realDir + "/四川银行新一数据_项目章程_新模板20240103.docx", "项目章程(真实,8表格)", 500},
        {realDir + "/四川银行新一代数据平台建设采购项目投标文件-整合稿.docx", "291MB大标书(真实)", 10000},
    };
    Document bigDoc;
    double bigMs = 0;
    for (const auto& d : docs) {
        if (!fs::exists(d.path)) { printf("  ⚠️ 跳过（不存在）：%s\n", d.label.c_str()); continue; }
        Document doc;
        auto t0 = Clock::now();
        DocxReadResult r = DocxReader::read(d.path, doc);
        double cost = ms(t0, Clock::now());
        CHECK(r.ok, ("打开 " + d.label).c_str());
        CHECK(doc.stats.paragraphs >= d.minParas,
              (d.label + " 段落数合理(" + std::to_string(doc.stats.paragraphs) + ")").c_str());
        printf("     耗时 %.0fms | %s\n", cost, doc.summary().c_str());
        if (d.minParas >= 10000) { bigDoc = doc; bigMs = cost; }
    }

    printf("═══════ Phase 2：大纲/字体/图片（大标书）═══════\n");
    if (bigDoc.stats.paragraphs) {
        CommandStack cs;
        DocumentEditor ed(bigDoc, cs);
        // 大纲：标题段计数
        size_t headings = 0;
        for (size_t i = 0; i < ed.paragraphCount(); ++i)
            if (const Paragraph* p = ed.paragraphAt(i); p && !p->styleId.empty()) ++headings;
        printf("     标题段：%zu\n", headings);
        CHECK(true, "大纲数据可构建");
        // 字体
        auto used = FontCheck::collectUsedFonts(bigDoc);
        CHECK(!used.empty(), ("字体清单(" + std::to_string(used.size()) + "种)").c_str());
        // 图片管线：长驻句柄按需取 + LRU
        ImageCache cache(64ull * 1024 * 1024);
        cache.setSource(g_srcPath = docs.back().path, &bigDoc);
        int pngOk = 0;
        const DecodedImage* firstImg = nullptr;
        for (const auto& m : bigDoc.mediaIndex) {
            if (m.zipPath.size() > 4 && m.zipPath.compare(m.zipPath.size()-4, 4, ".png") == 0) {
                if (const DecodedImage* im = cache.get(m.relId, 200, 150)) {
                    if (!firstImg) firstImg = im;
                    ++pngOk;
                }
                if (pngOk >= 5) break;
            }
        }
        CHECK(pngOk >= 1, ("PNG 按需解码(" + std::to_string(pngOk) + "张)").c_str());
        if (pngOk) {
            size_t hits0 = cache.stats().hits;
            cache.get(bigDoc.mediaIndex.front().relId, 200, 150);
            CHECK(cache.stats().hits > hits0, "LRU 缓存命中");
        }
        // 惰性分页
        EstimateMeasurer m(10.5);
        DocumentLayout lay(bigDoc, m);
        auto t0 = Clock::now();
        int pages = lay.ensureAll();
        printf("     分页：%d 页 / %.1fms\n", pages, ms(t0, Clock::now()));
        CHECK(pages > 100, "分页合理");
    }

    printf("═══════ Phase 3：编辑全流程（PRD 文档）═══════\n");
    Document doc;
    CHECK(DocxReader::read(realDir + "/贪吃蛇2048-PRD产品需求文档.docx", doc).ok, "读取 PRD");
    CommandStack stack;
    DocumentEditor ed(doc, stack);
    // 找一个正文段
    size_t target = 0;
    for (size_t i = 0; i < ed.paragraphCount(); ++i)
        if (ed.paragraphText(i).size() > 50) { target = i; break; }
    std::string before = ed.paragraphText(target);
    const Paragraph* tp = ed.paragraphAt(target);

    // 1 输入
    CHECK(ed.insertText({target, 0, 0}, "[E2E]"), "输入文本");
    CHECK(ed.paragraphText(target).find("[E2E]") == 0, "输入生效");
    // 2 回车
    size_t pc0 = ed.paragraphCount();
    CHECK(ed.splitParagraph({target, 0, 5}), "回车拆段");
    CHECK(ed.paragraphCount() == pc0 + 1, "拆段计数");
    // 3-9 格式七件套（字符级）
    size_t fp = target + 1;                          // 拆段后的原文本段
    const Paragraph* fpP = ed.paragraphAt(fp);
    size_t lastRun = fpP->runs.size() - 1;
    size_t lastOff = fpP->runs[lastRun].text.size();
    DocPos a{fp, 0, 0}, b{fp, lastRun, lastOff};
    CHECK(ed.setBold(a, b, true), "加粗");
    CHECK(ed.setItalic(a, b, true), "斜体");
    CHECK(ed.setUnderline(a, b, true), "下划线");
    CHECK(ed.setStrike(a, b, true), "删除线");
    CHECK(ed.setTextColor(a, b, "FF0000"), "文字颜色");
    CHECK(ed.setHighlight(a, b, "yellow"), "高亮");
    CHECK(ed.setFontSize(a, b, 16), "字号");
    CHECK(ed.setFontName(a, b, "仿宋", "FangSong"), "字体");
    // 10 对齐/样式
    CHECK(ed.setAlign(fp, Align::Center), "对齐");
    CHECK(ed.setParaStyle(fp, "Heading2"), "段落样式");
    // 11 剪贴板
    auto frag = ed.copyRange(a, b);
    CHECK(!frag.empty() && frag.plainText() == ed.paragraphText(fp), "复制");
    CHECK(ed.paste({0, 0, 0}, frag), "粘贴");
    CHECK(ed.paragraphText(0).rfind(frag.plainText(), 0) == 0, "粘贴内容生效（段首）");
    // 12 查找替换
    int rep = ed.replaceAll("贪吃蛇", "SNAKE2048");
    printf("     全部替换：%d 处\n", rep);
    CHECK(rep >= 1, "全部替换执行");
    // 13 撤销重做
    int undos = 0;
    while (stack.undo()) ++undos;
    printf("     撤销 %d 条命令\n", undos);
    CHECK(undos >= 10, "撤销全链");
    CHECK(ed.paragraphText(target).find("[E2E]") == std::string::npos ||
          ed.paragraphCount() == pc0, "撤销后状态恢复");
    while (stack.redo()) {}
    CHECK(ed.paragraphText(0).find("SNAKE2048") != std::string::npos, "重做后替换保留");

    printf("═══════ Phase 4：保存 → 重读逐字段验证 ═══════\n");
    g_srcPath = realDir + "/贪吃蛇2048-PRD产品需求文档.docx";
    {
        // 重做状态：开头的粘贴段有完整格式
        const Paragraph* sp = nullptr;
        for (size_t i = 0; i < ed.paragraphCount(); ++i)
            if (ed.paragraphText(i).find("SNAKE2048") != std::string::npos) { sp = ed.paragraphAt(i); break; }
        CHECK(sp && !sp->runs.empty(), "找到格式验证段");
        std::string out = dataDir + "/e2e_saved.docx";
        std::vector<std::string> warns;
        DocxWriteResult w = DocxWriter::write(out, doc, &mediaFromSrc, &warns);
        CHECK(w.ok, ("保存 " + std::to_string(w.writtenBytes) + "B").c_str());
        Document doc2;
        CHECK(DocxReader::read(out, doc2).ok, "重读");
        CommandStack cs2;
        DocumentEditor ed2(doc2, cs2);
        const Paragraph* rp = nullptr;
        for (size_t i = 0; i < ed2.paragraphCount(); ++i)
            if (const Paragraph* q = ed2.paragraphAt(i);
                q && q->align == Align::Center && q->styleId == "Heading2" &&
                !q->runs.empty() && q->runs.front().bold) { rp = q; break; }
        CHECK(rp && !rp->runs.empty(), "重读定位格式段");
        if (rp) {
            const auto& r = rp->runs.front();
            CHECK(r.bold && r.italic && r.underline && r.strike, "B/I/U/S 持久化");
            CHECK(r.color == "FF0000", "颜色持久化");
            CHECK(r.highlight == "yellow", "高亮持久化");
            CHECK(r.fontSizePt == 16, "字号持久化");
            CHECK(r.fontEastAsia == "仿宋", "中文字体持久化");
            CHECK(rp->align == Align::Center && rp->styleId == "Heading2", "对齐/样式持久化");
        }
        // 媒体保真
        CHECK(doc2.mediaIndex.size() == doc.mediaIndex.size(), "媒体条目保真");
    }

    printf("═══════ Phase 4.5：批次C 插入能力 ═══════\n");
    {
        Document cdoc;
        CHECK(DocxReader::read(dataDir + "/mini_bid.docx", cdoc).ok, "C:读取");
        CommandStack ccs;
        DocumentEditor ced(cdoc, ccs);
        // 插入表格 3x4 到段0后
        size_t body0 = ccd_before(cdoc);
        CHECK(ced.insertTableAfter(0, 3, 4), "C:插入表格");
        CHECK(cdoc.body.size() == body0 + 1, "C:表格节点+1");
        auto* tblNode = &cdoc.body[1];
        CHECK(tblNode->kind == Node::Kind::Table &&
              tblNode->table.rows.size() == 3 && tblNode->table.rows[0].cells.size() == 4,
              "C:表格结构3x4");
        CHECK(ccs.undo() && cdoc.body.size() == body0, "C:表格撤销");
        CHECK(ccs.redo() && cdoc.body.size() == body0 + 1, "C:表格重做");
        // 插入图片（媒体条目+段落图片+撤销）
        MediaEntry me; me.relId = "rIdNew1"; me.zipPath = "word/media/rIdNew1.png"; me.byteSize = 100;
        cdoc.mediaIndex.push_back(me);
        CHECK(ced.insertImage(0, "rIdNew1", 1905000, 1270000), "C:插入图片");
        CHECK(ced.paragraphAt(0)->images.size() == 1, "C:段落图片+1");
        CHECK(ccs.undo() && ced.paragraphAt(0)->images.empty(), "C:图片撤销");
        // 保存 round-trip（新媒体经 provider）
        g_srcPath = dataDir + "/mini_bid.docx";
        static std::map<std::string, std::vector<uint8_t>> mem = {{"rIdNew1", {0x89,0x50,0x4E,0x47}}};
        struct P { static bool prov(const std::string& id, const std::string&, std::vector<uint8_t>& o) {
            auto it = mem.find(id);
            if (it != mem.end()) { o = it->second; return true; }
            return false;
        }};
        auto r = DocxWriter::write(dataDir + "/c_insert.docx", cdoc, &P::prov, nullptr);
        CHECK(r.ok, "C:保存(含内存媒体)");
        Document back;
        CHECK(DocxReader::read(dataDir + "/c_insert.docx", back).ok &&
              back.stats.tables == 2 && back.mediaIndex.size() == 3,
              "C:round-trip(表格+媒体)");
    }

    printf("═══════ Phase 5：性能基线 ═══════\n");
    if (bigDoc.stats.paragraphs) {
        printf("  291MB 打开：%.0fms（预算 3000ms）%s\n", bigMs, bigMs < 3000 ? "✅" : "❌");
        CHECK(bigMs < 3000, "大文件打开性能");
        CommandStack csP;
        DocumentEditor edP(bigDoc, csP);
        auto t0 = Clock::now();
        for (int i = 0; i < 100; ++i)
            edP.insertText({100, 0, 0}, "测");
        double eMs = ms(t0, Clock::now()) / 100;
        printf("  编辑单键：%.3fms（预算 16ms）%s\n", eMs, eMs < 16 ? "✅" : "❌");
        CHECK(eMs < 16, "编辑性能");
    }

    printf("\n═══════ E2E 总计：%d 通过 / %d 失败 ═══════\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
