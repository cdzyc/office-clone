// oc::ImageCache — 图片按需解码 + LRU 字节预算缓存（ADR A5 落地）
// 关键行为：
//  1. 文档打开时绝不解码（索引已在 Document.mediaIndex）；
//  2. get(relId, targetW) 才按需解码 + 盒式下采样到目标尺寸；
//  3. LRU 按字节预算淘汰（缩略图与全图分开缓存条目）；
//  4. JPEG 等非 PNG 经注入的 ExternalDecoder（Windows: WIC/libjpeg-turbo）。
#pragma once
#include "Document.h"
#include "PngDecode.h"
#include "ZipReader.h"
#include <functional>
#include <list>
#include <string>
#include <unordered_map>

namespace oc {

class ImageCache {
public:
    using ExternalDecoder = std::function<bool(const std::vector<uint8_t>& file,
                                               DecodedImage& out)>;

    explicit ImageCache(size_t byteBudget = 384ull * 1024 * 1024);

    // 打开源文档：长驻 ZIP 句柄（索引模式）——取图无重开 IO 放大（P0 修复）
    void setSource(const std::string& docxPath, const Document* doc);
    void setExternalDecoder(ExternalDecoder dec) { external_ = std::move(dec); }

    // 按需获取（解码+下采样+缓存；未命中/不支持返回 nullptr）
    const DecodedImage* get(const std::string& relId, uint32_t targetW, uint32_t targetH = 0);

    struct Stats {
        size_t hits = 0, misses = 0, evictions = 0, unsupported = 0, decodeErrors = 0;
        size_t bytesCached = 0, entries = 0;
        double lastReadMs = 0, lastDecodeMs = 0;      // 取图耗时画像（调优用）
    };
    const Stats& stats() const { return st_; }
    size_t byteBudget() const { return budget_; }

private:
    struct Entry {
        std::string key;
        DecodedImage img;
    };
    void evictIfNeeded();
    static std::string makeKey(const std::string& relId, uint32_t w, uint32_t h);

    size_t budget_;
    std::list<Entry> lru_;                                // front = 最近使用
    std::unordered_map<std::string, std::list<Entry>::iterator> map_;
    std::string srcPath_;
    const Document* doc_ = nullptr;
    ZipReader zip_;                                       // 长驻句柄（索引模式）
    bool zipOk_ = false;
    ExternalDecoder external_;
    Stats st_;
};

} // namespace oc
