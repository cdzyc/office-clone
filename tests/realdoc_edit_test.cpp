// realdoc_edit_test — 真实标书编辑功能实测（M2/M2.6 全链路）
// 增(insert)/删(deleteRange)/改(setBold/setAlign)/拆段/复制粘贴/撤销重做
// → 写回 docx → 重读验证持久化。用法：./realdoc_edit_test <标书.docx>
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/EditCommands.h"
#include "../core/Clipboard.h"
#include "../core/CommandStack.h"
#include "../core/ZipReader.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace oc;
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; printf("  ✅ %s\n", msg); } \
    else { ++g_fail; printf("  ❌ %s (line %d)\n", msg, __LINE__); } } while (0)

static std::map<std::string, std::vector<uint8_t>> g_media;
static bool mediaFromMap(const std::string&, const std::string& zipPath,
                         std::vector<uint8_t>& out) {
    auto it = g_media.find(zipPath);
    if (it == g_media.end()) return false;
    out = it->second;
    return true;
}

static const char* kMarker = "[OC-EDIT-已验证] ";

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: realdoc_edit_test <docx>\n"); return 1; }
    std::string src = argv[1];

    printf("═══ 1. 读取文档 ═══\n");
    Document doc;
    DocxReadResult r = DocxReader::read(src, doc);
    CHECK(r.ok, "读取 291.7MB 真实标书成功");
    if (!r.ok) return 1;
    CommandStack stack;
    DocumentEditor ed(doc, stack);

    // 选前若干非空段落作为操作目标
    std::vector<size_t> paras;
    for (size_t i = 0; i < ed.paragraphCount() && paras.size() < 8; ++i)
        if (!ed.paragraphText(i).empty()) paras.push_back(i);
    CHECK(paras.size() >= 6, "找到非空操作段落");
    if (paras.size() < 6) return 1;
    auto& P = paras;

    // 快照（undo 验证基准）
    std::vector<std::string> snapshot;
    for (size_t p : P) snapshot.push_back(ed.paragraphText(p));
    Align origAlign = ed.paragraphAt(P[1])->align;      // 真实文档原始对齐（可能是任意值）

    printf("═══ 2. 增：插入文本 ═══\n");
    DocPos pos0{P[0], 0, 0};
    CHECK(ed.insertText(pos0, kMarker), "insertText 命令执行");
    CHECK(ed.paragraphText(P[0]).find(kMarker) == 0, "插入内容立即生效（段首）");

    printf("═══ 3. 改：加粗 + 对齐 ═══\n");
    const Paragraph* pp = ed.paragraphAt(P[1]);
    size_t lastRun = pp && !pp->runs.empty() ? pp->runs.size() - 1 : 0;
    CHECK(ed.setBold({P[1], 0, 0}, {P[1], lastRun, 0}, true), "setBold 命令执行");
    pp = ed.paragraphAt(P[1]);
    CHECK(pp && !pp->runs.empty() && pp->runs[0].bold, "加粗立即生效");
    CHECK(ed.setAlign(P[1], Align::Center), "setAlign 命令执行");
    CHECK(ed.paragraphAt(P[1])->align == Align::Center, "对齐立即生效");

    printf("═══ 4. 拆段（回车语义）═══\n");
    const Paragraph* sp = ed.paragraphAt(P[2]);
    size_t midRun = sp && !sp->runs.empty() ? 0 : 0;
    size_t midOff = sp && !sp->runs.empty() ? sp->runs[0].text.size() / 2 : 0;
    std::string before = ed.paragraphText(P[2]);
    size_t countBefore = ed.paragraphCount();
    CHECK(ed.splitParagraph({P[2], midRun, midOff}), "splitParagraph 命令执行");
    CHECK(ed.paragraphCount() == countBefore + 1, "拆段后段数 +1");
    CHECK(ed.paragraphText(P[2]) + ed.paragraphText(P[2] + 1) == before,
          "拆段内容无丢失（前+后=原文）");

    printf("═══ 5. 复制 → 粘贴 ═══\n");
    // 复制 P[3] 全文，粘贴到 P[4] 开头
    const Paragraph* cp = ed.paragraphAt(P[3]);
    size_t cpLast = cp && !cp->runs.empty() ? cp->runs.size() - 1 : 0;
    size_t cpLen = cp && !cp->runs.empty() ? cp->runs[cpLast].text.size() : 0;
    auto frag = ed.copyRange({P[3], 0, 0}, {P[3], cpLast, cpLen});
    CHECK(!frag.empty(), "copyRange 提取内容");
    CHECK(frag.plainText() == ed.paragraphText(P[3]), "复制内容与源一致");
    std::string targetBefore = ed.paragraphText(P[4]);
    CHECK(ed.paste({P[4], 0, 0}, frag), "paste 命令执行");
    CHECK(ed.paragraphText(P[4]).find(frag.plainText()) == 0, "粘贴内容在目标段首");
    CHECK(ed.paragraphText(P[4]) == frag.plainText() + targetBefore, "粘贴后原文保留在尾部");

    printf("═══ 6. 删：跨段删除 ═══\n");
    // 在 P[5] 段内删除中间 1/3（同段 deleteRange；跨段删除在 m2_tests 已验证，
    // 此处真实文件上再验一次同段删除的真实效果）
    const Paragraph* dp = ed.paragraphAt(P[5]);
    std::string p5 = ed.paragraphText(P[5]);
    if (dp && !dp->runs.empty()) {
        // 简化：删除 run0 的前 1/3 字节（保持 UTF-8 边界：找边界）
        size_t len = dp->runs[0].text.size();
        size_t cut = len / 3;
        // 回退到 UTF-8 边界
        while (cut < len && (dp->runs[0].text[cut] & 0xC0) == 0x80) ++cut;
        CHECK(ed.deleteRange({P[5], 0, 0}, {P[5], 0, cut}), "deleteRange 命令执行");
        CHECK(ed.paragraphText(P[5]) == p5.substr(cut), "删除后内容正确（前缀移除）");
    }

    printf("═══ 7. 撤销 → 全恢复 ═══\n");
    int undos = 0;
    while (stack.undo()) ++undos;
    printf("  共撤销 %d 条命令\n", undos);
    CHECK(undos >= 5, "撤销计数正确（≥5 条命令）");
    bool allRestored = true;
    for (size_t i = 0; i < P.size(); ++i)
        if (ed.paragraphText(P[i]) != snapshot[i]) allRestored = false;
    CHECK(allRestored, "全部段落文本恢复原状");
    CHECK(ed.paragraphAt(P[1])->align == origAlign, "对齐恢复原状（对比真实原始值）");
    CHECK(ed.paragraphCount() == countBefore, "段数恢复（拆段撤销）");

    printf("═══ 8. 重做 → 全应用 ═══\n");
    int redos = 0;
    while (stack.redo()) ++redos;
    CHECK(redos == undos, "重做计数一致");
    CHECK(ed.paragraphText(P[0]).find(kMarker) == 0, "插入恢复");
    CHECK(ed.paragraphAt(P[1])->align == Align::Center, "对齐恢复");

    printf("═══ 9. 写回 + 重读验证持久化 ═══\n");
    {
        auto t0 = Clock::now();
        ZipReader zr;
        zr.open(src);
        for (const auto& me : doc.mediaIndex) {
            const ZipEntry* e = zr.find(me.zipPath);
            if (!e) continue;
            std::vector<uint8_t> f;
            if (zr.read(*e, f)) g_media[me.zipPath] = std::move(f);
        }
        printf("  媒体载入: %.0f ms\n", msSince(t0));

        t0 = Clock::now();
        std::vector<std::string> warns;
        DocxWriteResult wr = DocxWriter::write("/tmp/opencode/realdoc_edited.docx", doc,
                                                &mediaFromMap, &warns);
        CHECK(wr.ok, ("写回成功: " + wr.error).c_str());
        if (!wr.ok) return 1;
        printf("  写回: %.1f s → %.1f MB\n", msSince(t0) / 1000.0, wr.writtenBytes / 1048576.0);

        t0 = Clock::now();
        Document doc2;
        DocxReadResult r2 = DocxReader::read("/tmp/opencode/realdoc_edited.docx", doc2);
        CHECK(r2.ok, "重读成功");
        if (!r2.ok) return 1;
        CommandStack cs2;
        DocumentEditor ed2(doc2, cs2);

        // 持久化断言：搜索标记
        bool markerFound = false, pasteFound = false;
        for (size_t i = 0; i < ed2.paragraphCount() && i < 50; ++i) {
            std::string t = ed2.paragraphText(i);
            if (t.find(kMarker) != std::string::npos) markerFound = true;
            if (t.find(frag.plainText()) == 0 && t.find(kMarker) == std::string::npos
                && !frag.plainText().empty())
                pasteFound = true;
        }
        CHECK(markerFound, "插入标记持久化（写回→重读可见）");
        CHECK(pasteFound, "粘贴内容持久化（写回→重读可见）");
        CHECK(doc2.stats.tables == doc.stats.tables, "表格数不变");
        CHECK(doc2.mediaIndex.size() == doc.mediaIndex.size(), "媒体数不变");
        printf("  重读: %.0f ms | %s\n", msSince(t0), doc2.summary().c_str());
    }

    printf("\n═══ 编辑功能实测: %d 通过 / %d 失败 ═══\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
