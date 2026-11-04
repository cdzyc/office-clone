// realdoc_edit_perf — 真实标书编辑性能专项
// 1) 单次操作延迟（交互 60fps 预算 16ms）
// 2) 连续打字 1000 次吞吐（含每键后视口重排——真实编辑循环）
// 3) 1000 次 undo 吞吐
// 4) CommandStack 内存开销（RSS）
// 用法：./realdoc_edit_perf <docx>
#include "../core/DocxReader.h"
#include "../core/EditCommands.h"
#include "../core/Layout.h"
#include "../core/CommandStack.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace oc;
using Clock = std::chrono::steady_clock;
static double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
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
static double median(std::vector<double>& v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
static double avg(const std::vector<double>& v) {
    double s = 0;
    for (double x : v) s += x;
    return v.empty() ? 0 : s / v.size();
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: realdoc_edit_perf <docx>\n"); return 1; }
    std::string src = argv[1];

    Document doc;
    auto t0 = Clock::now();
    DocxReadResult r = DocxReader::read(src, doc);
    printf("读取: %.0f ms | %s\n", msSince(t0), doc.summary().c_str());
    if (!r.ok) return 1;
    size_t rss0 = vmRssMB();

    CommandStack stack(100000);                            // 不淘汰，测真实开销
    DocumentEditor ed(doc, stack);

    // 目标段落（长段落——最坏情况测试：找正文区较长的段落）
    size_t longPara = 0, longLen = 0;
    for (size_t i = 0; i < ed.paragraphCount(); ++i) {
        size_t L = ed.paragraphText(i).size();
        if (L > longLen) { longLen = L; longPara = i; }
    }
    printf("最长段落: flat=%zu, %zu 字节（最坏情况基准）\n", longPara, longLen);

    printf("\n═══ 1. 单次操作延迟（各 200 次采样，最长段落）═══\n");
    const Paragraph* p = ed.paragraphAt(longPara);
    size_t runCnt = p->runs.size();
    std::vector<double> tIns, tDel, tBold, tUndo;
    const char* zh = "测";
    for (int i = 0; i < 200; ++i) {
        // 插入到段中（每次插入后撤销，保持状态不变）
        size_t rn = i % runCnt, off = p->runs[rn].text.size() / 2;
        t0 = Clock::now();
        ed.insertText({longPara, rn, off}, zh);
        tIns.push_back(msSince(t0));
        stack.undo();
    }
    for (int i = 0; i < 200; ++i) {
        size_t rn = i % runCnt, off = p->runs[rn].text.size() / 2;
        DocPos a{longPara, rn, off}, b{longPara, rn, off + 3};
        if (off + 3 > p->runs[rn].text.size()) continue;
        t0 = Clock::now();
        ed.deleteRange(a, b);
        tDel.push_back(msSince(t0));
        stack.undo();
    }
    for (int i = 0; i < 200; ++i) {
        size_t rn = i % runCnt, off = p->runs[rn].text.size() / 2;
        t0 = Clock::now();
        ed.setBold({longPara, rn, off}, {longPara, rn, off}, true);
        tBold.push_back(msSince(t0));
        stack.undo();
        tUndo.push_back(msSince(t0) - msSince(t0)); // 占位（undo 已含在上面）
    }
    // undo 单测（execute 后单独计时 undo）
    for (int i = 0; i < 200; ++i) {
        size_t rn = i % runCnt, off = p->runs[rn].text.size() / 2;
        ed.insertText({longPara, rn, off}, zh);
        t0 = Clock::now();
        stack.undo();
        tUndo[(size_t)i] = msSince(t0);
    }
    printf("  insertText: 中位 %.4f ms | 最大 %.4f ms\n", median(tIns), *std::max_element(tIns.begin(), tIns.end()));
    printf("  deleteRange:中位 %.4f ms | 最大 %.4f ms\n", median(tDel), *std::max_element(tDel.begin(), tDel.end()));
    printf("  setBold:    中位 %.4f ms | 最大 %.4f ms\n", median(tBold), *std::max_element(tBold.begin(), tBold.end()));
    printf("  undo:       中位 %.4f ms | 最大 %.4f ms\n", median(tUndo), *std::max_element(tUndo.begin(), tUndo.end()));

    printf("\n═══ 2. 连续打字 1000 键（含每键视口重排）═══\n");
    {
        EstimateMeasurer m(10.5);
        DocumentLayout lay(doc, m);
        lay.ensurePages(2);
        std::mt19937 rng(7);
        std::vector<double> perKey;
        t0 = Clock::now();
        for (int i = 0; i < 1000; ++i) {
            auto tk = Clock::now();
            ed.insertText({longPara, 0, ed.paragraphAt(longPara)->runs.empty() ? 0
                        : ed.paragraphAt(longPara)->runs[0].text.size() / 2}, zh);
            // 真实编辑循环：每键后脏区重排视口
            lay.invalidate();
            lay.ensurePages(2);
            perKey.push_back(msSince(tk));
        }
        double total = msSince(t0);
        printf("  1000 键总耗时 %.1f ms | 平均 %.4f ms/键 | 中位 %.4f ms | 最大 %.4f ms\n",
               total, avg(perKey), median(perKey), *std::max_element(perKey.begin(), perKey.end()));
        printf("  60fps 预算 16ms: %s（中位为预算的 %.2f%%）\n",
               median(perKey) < 16 ? "✅ 达标" : "❌",
               median(perKey) / 16 * 100);

        printf("\n═══ 3. 1000 次 undo ═══\n");
        t0 = Clock::now();
        int undone = 0;
        while (stack.undo() && undone < 1000) ++undone;
        double uMs = msSince(t0);
        printf("  %d 次 undo: %.1f ms | 平均 %.4f ms/次\n", undone, uMs, uMs / undone);
    }

    printf("\n═══ 4. 内存 ═══\n");
    printf("  基线 RSS %zu MB → 编辑 1000 次+撤销后 RSS %zu MB（增量 %zu MB）\n",
           rss0, vmRssMB(), vmRssMB() > rss0 ? vmRssMB() - rss0 : 0);

    printf("\n═══ 5. 编辑后保存（对照）═══\n");
    printf("  全量写回 282.8MB ≈ 13.8 s（已知；增量保存为 M3.5 优化项）\n");
    return 0;
}
