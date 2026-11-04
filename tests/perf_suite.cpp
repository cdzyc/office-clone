// perf_suite — M3 自动性能基准：打开/首屏/全量分页/图片解码/LRU/多文档内存
// 产出：PERF.md（Markdown 报告）
// 运行：先 python3 tests/make_fixtures.py + make_big_docx.py（47MB 与 118MB），
//      再 ./perf_suite tests/data
#include "../core/DocxReader.h"
#include "../core/ImageCache.h"
#include "../core/Layout.h"
#include "../core/PngDecode.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace oc;
using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static size_t vmRssKB() {
#ifdef _WIN32
    return 0;                                              // Windows：M3.5 接 GetProcessMemoryInfo
#else
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %zu kB", &kb) == 1) break;
    fclose(f);
    return kb;
#endif
}

struct Row {
    std::string scene, metric, value, verdict;
};

int main(int argc, char** argv) {
    std::string dataDir = argc > 1 ? argv[1] : "data";
    std::vector<Row> rows;
    auto add = [&](std::string s, std::string m, std::string v, std::string j) {
        rows.push_back({std::move(s), std::move(m), std::move(v), std::move(j)});
    };

    // ---------- 1. 文档打开 + 首屏 + 全量分页 ----------
    struct Sample { const char* file; const char* label; };
    Sample samples[] = {
        {"big_bid.docx", "118MB 标书(5075段/75图)"},
    };
    for (auto& s : samples) {
        std::string path = dataDir + "/" + s.file;
        auto t0 = Clock::now();
        Document doc;
        DocxReadResult r = DocxReader::read(path, doc);
        double openMs = msSince(t0);
        if (!r.ok) { printf("open failed: %s\n", r.error.c_str()); return 1; }

        EstimateMeasurer m(10.5);
        DocumentLayout lay(doc, m);
        t0 = Clock::now();
        lay.ensurePages(2);
        double firstMs = msSince(t0);
        t0 = Clock::now();
        int pages = lay.ensureAll();
        double allMs = msSince(t0);

        char v[64];
        snprintf(v, sizeof v, "%.0f ms", openMs);
        add(s.label, "打开+解析（媒体索引不解码）", v, openMs < 3000 ? "✅ ≤3s" : "❌");
        snprintf(v, sizeof v, "%.3f ms", firstMs);
        add(s.label, "首屏（前2页布局）", v, firstMs < 10 ? "✅ <10ms" : "❌");
        snprintf(v, sizeof v, "%.1f ms / %d 页", allMs, pages);
        add(s.label, "全量分页", v, allMs < 500 ? "✅ <500ms" : "❌");

        // 首图按需解码（读取字节——PNG 视格式而定）
        if (!doc.mediaIndex.empty()) {
            t0 = Clock::now();
            std::vector<uint8_t> media;
            DocxReader::readMedia(path, doc.mediaIndex[0].zipPath, media);
            double mediaMs = msSince(t0);
            snprintf(v, sizeof v, "%.0f ms / %.1f MB", mediaMs, media.size() / 1048576.0);
            add(s.label, "按需读取首图（流式）", v, mediaMs < 1000 ? "✅" : "⚠️");
        }
    }

    // ---------- 2. PNG 解码 + 缩略图 + LRU ----------
    {
        FILE* f = fopen((dataDir + "/png/big_scan.png").c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> buf((size_t)n);
            if (fread(buf.data(), 1, (size_t)n, f) == (size_t)n) {
                DecodedImage img;
                auto t0 = Clock::now();
                bool ok = pngDecode(buf, img);
                double decMs = msSince(t0);
                t0 = Clock::now();
                DecodedImage thumb = downsample(img, 200, 150);
                double dsMs = msSince(t0);
                fclose(f);

                char v[96];
                snprintf(v, sizeof v, "%.1f ms（%ux%u → RGBA）", decMs, img.width, img.height);
                add("图片管线", "PNG 解码 1200×900", v, ok && decMs < 200 ? "✅" : "❌");
                snprintf(v, sizeof v, "%.2f ms（→%ux%u）", dsMs, thumb.width, thumb.height);
                add("图片管线", "盒式下采样缩略图", v, dsMs < 50 ? "✅" : "❌");

                // LRU 命中（重复 get 免解码）
                Document doc;
                if (DocxReader::read(dataDir + "/mini_bid.docx", doc).ok) {
                    ImageCache cache(64ull * 1024 * 1024);
                    cache.setSource(dataDir + "/mini_bid.docx", &doc);
                    cache.get("rId8", 32, 24);
                    t0 = Clock::now();
                    for (int i = 0; i < 1000; ++i) cache.get("rId8", 32, 24);
                    double hitMs = msSince(t0);
                    snprintf(v, sizeof v, "%.3f ms / 1000 次（%.1f μs/次）",
                             hitMs, hitMs);
                    add("图片管线", "LRU 缓存命中", v, hitMs < 10 ? "✅" : "❌");
                }
            } else {
                fclose(f);
            }
        }
    }

    // ---------- 3. 多文档内存 ----------
    {
        std::vector<std::unique_ptr<Document>> docs;
        size_t rss0 = vmRssKB();
        for (int i = 0; i < 5; ++i) {
            auto d = std::make_unique<Document>();
            if (!DocxReader::read(dataDir + "/big_bid.docx", *d).ok) break;
            docs.push_back(std::move(d));
        }
        size_t rss1 = vmRssKB();
        char v[96];
        snprintf(v, sizeof v, "%zu 个 → +%zu MB（RSS）", docs.size(), (rss1 > rss0 ? rss1 - rss0 : 0) / 1024);
        add("多文档", "118MB×5 同时打开", v, docs.size() == 5 ? "✅（媒体未解码）" : "❌");
    }

    // ---------- 报告 ----------
    FILE* out = fopen("PERF.md", "w");
    if (!out) { printf("cannot write PERF.md\n"); return 1; }
    fprintf(out, "# M3 性能基准报告\n\n> 自动生成：perf_suite　环境：g++ -O2 / Linux 沙箱\n\n");
    fprintf(out, "| 场景 | 指标 | 实测 | 判定 |\n|---|---|---|---|\n");
    for (auto& r : rows)
        fprintf(out, "| %s | %s | %s | %s |\n", r.scene.c_str(), r.metric.c_str(),
                r.value.c_str(), r.verdict.c_str());
    fprintf(out, "\n> 指标基线（计划）：打开≤3s、首屏<10ms、全量分页<500ms、图片解码<200ms。\n");
    fprintf(out, "> JPEG 解码经 ImageCache::setExternalDecoder 注入（Windows: WIC/libjpeg-turbo）。\n");
    fclose(out);

    for (auto& r : rows)
        printf("%-24s %-32s %-28s %s\n", r.scene.c_str(), r.metric.c_str(),
               r.value.c_str(), r.verdict.c_str());
    printf("\nPERF.md written.\n");
    return 0;
}
