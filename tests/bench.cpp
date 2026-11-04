// bench — 大标书打开性能冒烟测试
// 验证架构核心主张：媒体只建索引不解码 → 打开耗时与正文 XML 相关、与媒体总量弱相关。
#include "../core/DocxReader.h"
#include <chrono>
#include <cstdio>

using namespace oc;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "data/big_bid.docx";
    printf("opening: %s\n", path);

    auto t0 = Clock::now();
    Document doc;
    DocxReadResult r = DocxReader::read(path, doc);
    auto t1 = Clock::now();

    if (!r.ok) { printf("FAILED: %s\n", r.error.c_str()); return 1; }
    printf("open+parse: %.1f ms (%.2f s)\n", ms(t0, t1), ms(t0, t1) / 1000.0);
    printf("summary:   %s\n", doc.summary().c_str());

    // 模拟 UI 首屏：读取第一张图（按需解码路径）
    if (!doc.mediaIndex.empty()) {
        auto t2 = Clock::now();
        std::vector<uint8_t> media;
        std::string err;
        bool ok = DocxReader::readMedia(path, doc.mediaIndex[0].zipPath, media, &err);
        auto t3 = Clock::now();
        printf("first media: %s %zu bytes in %.1f ms\n", ok ? "ok" : err.c_str(),
               media.size(), ms(t2, t3));
    }
    return 0;
}
