// m3_tests — M3 模块测试：PngDecode / downsample / ImageCache(LRU) / Layout(惰性分页)
// 运行：先 python3 tests/make_fixtures.py，再 ./m3_tests [dataDir]
#include "../core/PngDecode.h"
#include "../core/ImageCache.h"
#include "../core/Layout.h"
#include "../core/DocxReader.h"
#include "../core/Document.h"
#include <chrono>
#include <cstdio>
#include <cstring>
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

static void testPng(const std::string& dataDir) {
    std::string dir = dataDir + "/png";
    struct Case { const char* name; uint32_t w, h; };
    Case cases[] = {
        {"rgb_mixed.png", 64, 48}, {"rgba_none.png", 80, 50},
        {"gray_paeth.png", 33, 21}, {"graya_up.png", 40, 30},
        {"big_scan.png", 1200, 900},
    };
    for (auto& c : cases) {
        auto buf = loadFile(dir + "/" + c.name);
        CHECK(!buf.empty(), ("png load " + std::string(c.name)).c_str());
        DecodedImage img;
        std::string err;
        bool ok = pngDecode(buf, img, &err);
        CHECK(ok, ("png decode " + std::string(c.name) + " " + err).c_str());
        CHECK(img.width == c.w && img.height == c.h, "png dims");
        CHECK(img.rgba.size() == (size_t)c.w * c.h * 4, "png rgba size");
    }
    // 像素级规律对拍（rgb_mixed: r=(x*7+y*3)&255）
    {
        auto buf = loadFile(dir + "/rgb_mixed.png");
        DecodedImage img;
        CHECK(pngDecode(buf, img), "px decode");
        bool pxOk = true;
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) {
                const uint8_t* p = img.rgba.data() + ((size_t)y * img.width + x) * 4;
                if (p[0] != ((x * 7 + y * 3) & 255) ||
                    p[1] != ((x * 13 + y * 11) & 255) ||
                    p[2] != ((x * 29 + y * 17) & 255) || p[3] != 255) { pxOk = false; }
            }
        CHECK(pxOk, "png pixel law");
    }
    // downsample 自洽对拍：缩小结果 vs 手工块平均
    {
        auto buf = loadFile(dir + "/rgb_mixed.png");
        DecodedImage img;
        CHECK(pngDecode(buf, img), "ds decode");
        DecodedImage small = downsample(img, 16, 12);
        CHECK(small.width == 16 && small.height == 12, "ds dims");
        // 手工算 (0,0)：源块 x∈[0,64/16)=[0,4)×4 … 实际盒映射 x0=y*W/w
        bool avgOk = true;
        for (int sy = 0; sy < 12; sy += 4)
            for (int sx = 0; sx < 16; sx += 5) {
                uint32_t x0 = (uint32_t)((uint64_t)sx * 64 / 16);
                uint32_t x1 = (uint32_t)((uint64_t)(sx + 1) * 64 / 16);
                uint32_t y0 = (uint32_t)((uint64_t)sy * 48 / 12);
                uint32_t y1 = (uint32_t)((uint64_t)(sy + 1) * 48 / 12);
                uint64_t r = 0; uint32_t cnt = 0;
                for (uint32_t y = y0; y < y1; ++y)
                    for (uint32_t x = x0; x < x1; ++x) {
                        r += img.rgba[((size_t)y * 64 + x) * 4];
                        ++cnt;
                    }
                uint8_t expect = (uint8_t)(r / cnt);
                if (small.rgba[((size_t)sy * 16 + sx) * 4] != expect) avgOk = false;
            }
        CHECK(avgOk, "ds box-average law");
    }
}

static void testImageCache(const std::string& dataDir) {
    std::string docx = dataDir + "/mini_bid.docx";
    Document doc;
    CHECK(DocxReader::read(docx, doc).ok, "cache read doc");
    ImageCache cache(8ull * 1024 * 1024);
    cache.setSource(docx, &doc);

    // rId8 = 真 PNG 64x48 → 解码 + 缩到 32 宽
    const DecodedImage* img = cache.get("rId8", 32, 24);
    CHECK(img && img->width == 32 && img->height == 24, "cache get png thumb");
    // 命中
    img = cache.get("rId8", 32, 24);
    CHECK(img && cache.stats().hits == 1, "cache hit");
    CHECK(cache.stats().misses == 1, "cache miss count");
    // 不同尺寸 → 新条目
    img = cache.get("rId8", 16, 12);
    CHECK(img && img->width == 16 && cache.stats().entries == 2, "cache second size");
    // 原尺寸（targetW=0 表示不下采样？约定 0=原图）—— get(relId,0) 走 downsample(0)→原图
    img = cache.get("rId8", 0, 0);
    CHECK(img && img->width == 64, "cache full size");
    // rId9 = 假 JPG，无外部解码器 → unsupported
    CHECK(cache.get("rId9", 32) == nullptr, "cache unsupported");
    CHECK(cache.stats().unsupported == 1, "cache unsupported stat");
    // 不存在的 relId
    CHECK(cache.get("nope", 32) == nullptr, "cache unknown rel");
    // LRU 淘汰：预算调到极小
    ImageCache tiny(1024);                                 // 1KB 预算 < 任意条目
    tiny.setSource(docx, &doc);
    tiny.get("rId8", 32, 24);
    size_t bytesAfterEvict = tiny.stats().bytesCached;
    CHECK(bytesAfterEvict == 0, "cache lru evict to zero");
    CHECK(tiny.stats().evictions >= 1, "cache eviction count");
}

static void testLayout() {
    // 构造 300 段
    Document doc;
    doc.body.clear();
    for (int i = 0; i < 300; ++i) {
        Node n{Node::Kind::Paragraph, Paragraph{}, Table{}};
        TextRun r;
        r.text = "第" + std::to_string(i) + "段：这是用于惰性分页引擎测试的中文正文内容，长度中等。";
        n.paragraph.runs = {r};
        doc.body.push_back(n);
    }
    EstimateMeasurer m(10.5);
    DocumentLayout lay(doc, m);

    // 惰性：首屏 2 页
    int got = lay.ensurePages(2);
    CHECK(got >= 2, "layout ensure 2 pages");
    CHECK(!lay.fullyLaid(), "layout not fully");
    CHECK(lay.page(0).paraCount > 0 && lay.page(1).paraCount > 0, "layout pages have paras");

    // 全量
    int all = lay.ensureAll();
    CHECK(lay.fullyLaid() && all == lay.knownPages(), "layout ensure all");
    // 全覆盖：sum(paraCount) == 300 且页区间连续
    int total = 0, first = 0;
    bool contig = true;
    for (int i = 0; i < all; ++i) {
        const auto& pc = lay.page(i);
        if (pc.firstPara != first) contig = false;
        total += pc.paraCount;
        first += pc.paraCount;
    }
    CHECK(total == 300, "layout covers all paras");
    CHECK(contig, "layout page spans contiguous");
    CHECK(lay.pageOfParagraph(0) == 0, "layout para0 page0");
    CHECK(lay.pageOfParagraph(299) == all - 1, "layout last para last page");

    // 失效重排一致
    int before = lay.knownPages();
    lay.invalidate();
    CHECK(lay.knownPages() == 0 && !lay.fullyLaid(), "layout invalidated");
    int after = lay.ensureAll();
    CHECK(after == before, "layout re-layout stable");

    // 首屏性能：invalidate 后 ensurePages(2) < 5ms
    lay.invalidate();
    auto t0 = std::chrono::steady_clock::now();
    lay.ensurePages(2);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 5.0, "layout first-screen <5ms");
    printf("  [perf] 300 paras first-2-pages: %.3f ms\n", ms);
}

static void testLayoutBigDoc(const std::string& dataDir) {
    // big_bid.docx（2000+ 段）
    Document doc;
    if (!DocxReader::read(dataDir + "/big_bid.docx", doc).ok) {
        CHECK(false, "bigdoc read");
        return;
    }
    EstimateMeasurer m(10.5);
    DocumentLayout lay(doc, m);
    auto t0 = std::chrono::steady_clock::now();
    lay.ensurePages(3);
    double first = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
    CHECK(first < 10.0, "bigdoc first-screen <10ms");

    t0 = std::chrono::steady_clock::now();
    int pages = lay.ensureAll();
    double full = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
    printf("  [perf] bigdoc(%zu paras): first-3-pages %.2f ms, all %d pages %.1f ms\n",
           (size_t)doc.stats.paragraphs, first, pages, full);
    CHECK(full < 500.0, "bigdoc layout-all <500ms");
    CHECK(pages > 10, "bigdoc pages plausible");
}

int main(int argc, char** argv) {
    std::string dataDir = argc > 1 ? argv[1] : "data";
    testPng(dataDir);
    testImageCache(dataDir);
    testLayout();
    testLayoutBigDoc(dataDir);
    printf("\n=== M3: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
