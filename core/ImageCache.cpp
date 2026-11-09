#include "ImageCache.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace oc {

ImageCache::ImageCache(size_t byteBudget) : budget_(byteBudget) {}

void ImageCache::setSource(const std::string& docxPath, const Document* doc) {
    srcPath_ = docxPath;
    doc_ = doc;
    lru_.clear();
    map_.clear();
    st_ = Stats{};
    // P0 修复：长驻索引句柄——中央目录只解析一次，取图直接 fseek
    std::string err;
    zipOk_ = zip_.open(docxPath, &err);
    if (!zipOk_)
        std::fprintf(stderr, "ImageCache: open zip failed: %s\n", err.c_str());
}

void ImageCache::addMemoryMedia(const std::string& relId, std::vector<uint8_t> bytes) {
    memoryMedia_[relId] = std::move(bytes);
}

std::string ImageCache::makeKey(const std::string& relId, uint32_t w, uint32_t h) {
    return relId + "#" + std::to_string(w) + "x" + std::to_string(h);
}

void ImageCache::evictIfNeeded() {
    while (!lru_.empty() && st_.bytesCached > budget_) {
        auto victim = std::prev(lru_.end());               // LRU 尾 = 最久未用
        st_.bytesCached -= victim->img.bytes();
        map_.erase(victim->key);
        lru_.erase(victim);
        ++st_.evictions;
    }
}

const DecodedImage* ImageCache::get(const std::string& relId, uint32_t targetW, uint32_t targetH) {
    if (!doc_ || srcPath_.empty()) { ++st_.misses; return nullptr; }

    const MediaEntry* me = nullptr;
    for (const auto& m : doc_->mediaIndex)
        if (m.relId == relId) { me = &m; break; }
    if (!me) { ++st_.misses; return nullptr; }

    std::string key = makeKey(relId, targetW, targetH);
    auto it = map_.find(key);
    if (it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);      // 提升为最近使用
        ++st_.hits;
        return &it->second->img;
    }
    ++st_.misses;

    // 读取媒体字节：内存媒体（新插入）优先，其次长驻句柄
    std::vector<uint8_t> file;
    auto t0 = std::chrono::steady_clock::now();
    auto memIt = memoryMedia_.find(relId);
    bool have = false;
    if (memIt != memoryMedia_.end()) {
        file = memIt->second;
        have = true;
    } else if (zipOk_) {
        const ZipEntry* ze = zip_.find(me->zipPath);
        if (ze && zip_.read(*ze, file, 512ull * 1024 * 1024)) have = true;
    }
    if (!have) { ++st_.decodeErrors; return nullptr; }
    auto t1 = std::chrono::steady_clock::now();

    DecodedImage img;
    bool ok = false;
    if (file.size() >= 8 && file[0] == 0x89 && file[1] == 0x50)   // PNG
        ok = pngDecode(file, img);
    else if (external_)                                            // JPEG 等
        ok = external_(file, img);
    if (!ok) { ++st_.unsupported; return nullptr; }
    auto t2 = std::chrono::steady_clock::now();
    st_.lastReadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    st_.lastDecodeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

    if (targetW) img = downsample(img, targetW, targetH ? targetH : targetW);

    Entry e;
    e.key = key;
    e.img = std::move(img);
    lru_.push_front(std::move(e));
    map_[key] = lru_.begin();
    st_.bytesCached += lru_.front().img.bytes();
    ++st_.entries;
    evictIfNeeded();
    return &lru_.front().img;
}

} // namespace oc
