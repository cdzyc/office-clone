// realdoc_test — 真实标书综合实测：性能 + 兼容性（字体/PNG解码/结构对拍/round-trip）
// 用法：./realdoc_test <标书.docx>
#include "../core/DocxReader.h"
#include "../core/DocxWriter.h"
#include "../core/FontCheck.h"
#include "../core/ImageCache.h"
#include "../core/Layout.h"
#include "../core/PngDecode.h"
#include "../core/ZipReader.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace oc;
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
static size_t vmRssMB() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256]; size_t kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %zu kB", &kb) == 1) break;
    fclose(f);
    return kb / 1024;
}

// 媒体内存 map（避免 readMedia 每次重开 292MB zip——IO 放大问题已识别为 M3.5 优化点）
static std::map<std::string, std::vector<uint8_t>> g_mediaMap;
static bool mediaFromMap(const std::string&, const std::string& zipPath,
                         std::vector<uint8_t>& out) {
    auto it = g_mediaMap.find(zipPath);
    if (it == g_mediaMap.end()) return false;
    out = it->second;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: realdoc_test <docx>\n"); return 1; }
    std::string src = argv[1];

    printf("═══ 1. 打开 + 解析 ═══\n");
    auto t0 = Clock::now();
    Document doc;
    DocxReadResult r = DocxReader::read(src, doc);
    double openMs = msSince(t0);
    if (!r.ok) { printf("FAILED: %s\n", r.error.c_str()); return 1; }
    printf("  打开+解析: %.0f ms | RSS %zu MB\n", openMs, vmRssMB());
    printf("  %s\n", doc.summary().c_str());
    {
        // 聚合告警（900+ 图会刷屏）
        std::map<std::string, int> warnCount;
        for (auto& w : r.warnings) ++warnCount[w];
        for (auto& [w, n] : warnCount)
            printf("  warn ×%d: %s\n", n, w.c_str());
    }

    printf("═══ 2. 全量分页 ═══\n");
    EstimateMeasurer m(10.5);
    DocumentLayout lay(doc, m);
    t0 = Clock::now();
    int first = lay.ensurePages(2);
    double firstMs = msSince(t0);
    t0 = Clock::now();
    int pages = lay.ensureAll();
    double allMs = msSince(t0);
    printf("  首屏(2页): %.3f ms | 全量: %d 页 %.1f ms | %zu 段 → %.1f 段/页\n",
           firstMs, pages, allMs, (size_t)doc.stats.paragraphs,
           (double)doc.stats.paragraphs / pages);

    printf("═══ 3. 字体分析（FontCheck，模拟 Windows 自带集）═══\n");
    auto used = FontCheck::collectUsedFonts(doc);
    printf("  文档使用字体 %zu 种: ", used.size());
    for (size_t i = 0; i < used.size(); ++i)
        printf("%s%s", i ? "、" : "", used[i].c_str());
    printf("\n");
    // Windows 11 常见自带字体模拟集
    std::set<std::string> win11 = {
        "宋体", "SimSun", "黑体", "SimHei", "楷体", "KaiTi", "仿宋", "FangSong",
        "微软雅黑", "Microsoft YaHei", "等线", "DengXian", "Microsoft JhengHei",
        "Times New Roman", "Arial", "Calibri", "Cambria", "Courier New",
        "Segoe UI", "Tahoma", "Verdana", "MS Gothic", "MS Mincho",
        "Wingdings", "Symbol", "宋体", "新宋体", "NSimSun",
    };
    auto issues = FontCheck::check(used, win11);
    if (issues.empty()) printf("  ✅ 全部字体 Windows 自带\n");
    else {
        printf("  ⚠️ 疑似非自带字体 %zu 种（用户机器可能缺失）:\n", issues.size());
        for (auto& i : issues)
            printf("     - %s%s\n", i.missing.c_str(),
                   i.fallback.empty() ? "（无内置回退建议）"
                                      : ("→ 建议回退 " + i.fallback).c_str());
    }

    printf("═══ 4. 真实媒体抽样解码（PNG 前 30 个）═══\n");
    {
        // 一次性载媒体（识别 IO 放大问题的对照组）
        t0 = Clock::now();
        ZipReader zr;
        if (!zr.open(src)) { printf("  zip reopen fail\n"); return 1; }
        int pngOk = 0, pngFail = 0, nonPng = 0, tested = 0;
        std::string failReasons;
        double decMs = 0;
        for (const auto& me : doc.mediaIndex) {
            if (tested >= 30) break;
            size_t dot = me.zipPath.rfind('.');
            std::string ext = dot == std::string::npos ? "" : me.zipPath.substr(dot + 1);
            for (auto& c : ext) c = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            const ZipEntry* e = zr.find(me.zipPath);
            if (!e) continue;
            std::vector<uint8_t> file;
            if (!zr.read(*e, file)) continue;
            if (ext == "png") {
                DecodedImage img; std::string err;
                auto td = Clock::now();
                bool ok = pngDecode(file, img, &err);
                decMs += msSince(td);
                if (ok) { ++pngOk; if (img.width > 2000)
                    printf("     大图: %s %ux%u\n", me.zipPath.c_str(), img.width, img.height); }
                else { ++pngFail; failReasons += err + "; "; }
                ++tested;
            } else if (ext == "jpeg" || ext == "jpg") {
                ++nonPng;
                g_mediaMap[me.zipPath] = std::move(file);   // 留给 round-trip
            } else {
                ++nonPng;
                g_mediaMap[me.zipPath] = std::move(file);
            }
        }
        printf("  PNG 抽样 %d 个: 解码成功 %d, 失败 %d, 平均 %.1f ms/张\n",
               tested, pngOk, pngFail, tested ? decMs / tested : 0);
        if (pngFail) printf("  失败原因: %s\n", failReasons.c_str());
        printf("  （JPEG/EMF/WDP 等 %d 个走 ExternalDecoder 注入——Windows: WIC）\n", nonPng);
        double loadMs = msSince(t0);
        printf("  一次性载入抽样媒体耗时: %.0f ms（对照：单图 readMedia 重开 zip 需 ~270ms/次）\n",
               loadMs);
    }

    printf("═══ 5. Round-trip 写回 + 重读 ═══\n");
    {
        // 全量媒体载入 map（round-trip 用）
        t0 = Clock::now();
        ZipReader zr;
        zr.open(src);
        for (const auto& me : doc.mediaIndex) {
            const ZipEntry* e = zr.find(me.zipPath);
            if (!e) continue;
            std::vector<uint8_t> file;
            if (zr.read(*e, file)) g_mediaMap[me.zipPath] = std::move(file);
        }
        printf("  媒体全量载入: %.0f ms | RSS %zu MB\n", msSince(t0), vmRssMB());

        t0 = Clock::now();
        std::vector<std::string> warns;
        DocxWriteResult wr = DocxWriter::write("/tmp/opencode/realdoc_rt.docx", doc,
                                               &mediaFromMap, &warns);
        double writeMs = msSince(t0);
        if (!wr.ok) { printf("  写回失败: %s\n", wr.error.c_str()); return 1; }
        printf("  写回: %.1f s → %.1f MB\n", writeMs / 1000.0, wr.writtenBytes / 1048576.0);

        t0 = Clock::now();
        Document doc2;
        DocxReadResult r2 = DocxReader::read("/tmp/opencode/realdoc_rt.docx", doc2);
        double reopenMs = msSince(t0);
        if (!r2.ok) { printf("  重读失败: %s\n", r2.error.c_str()); return 1; }
        bool sameParas = doc2.stats.paragraphs == doc.stats.paragraphs;
        bool sameTables = doc2.stats.tables == doc.stats.tables;
        bool sameImages = doc2.stats.images == doc.stats.images;
        bool sameMedia = doc2.mediaIndex.size() == doc.mediaIndex.size();
        bool samePage = doc2.page.pageWidthTw == doc.page.pageWidthTw;
        printf("  重读: %.0f ms | 段落%s 表格%s 图片%s 媒体%s 页面%s\n",
               reopenMs, sameParas ? "✅" : "❌", sameTables ? "✅" : "❌",
               sameImages ? "✅" : "❌", sameMedia ? "✅" : "❌", samePage ? "✅" : "❌");
        // 媒体字节抽查（首/中/尾 3 个）
        int byteOk = 0, byteChecked = 0;
        for (int idx : {0, (int)doc.mediaIndex.size() / 2, (int)doc.mediaIndex.size() - 1}) {
            std::vector<uint8_t> a, b;
            if (DocxReader::readMedia(src, doc.mediaIndex[idx].zipPath, a) &&
                DocxReader::readMedia("/tmp/opencode/realdoc_rt.docx", doc.mediaIndex[idx].zipPath, b)) {
                ++byteChecked;
                if (a == b) ++byteOk;
            }
        }
        printf("  媒体字节抽查: %d/%d 一致\n", byteOk, byteChecked);
        printf("  RSS 峰值: %zu MB\n", vmRssMB());
    }

    printf("\n═══ 完成 ═══\n");
    return 0;
}
