#include "ImageCache.h"
#include "DocxReader.h"
#include <algorithm>
#include <cstdio>

namespace oc {

ImageCache::ImageCache(size_t byteBudget) : budget_(byteBudget) {}

void ImageCache::setSource(const std::string& docxPath, const Document* doc) {
    srcPath_ = docxPath;
    doc_ = doc;
    lru_.clear();
    map_.clear();
    st_ = Stats{};
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

    // 查 mediaIndex
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

    // 读文件字节（M3：每次重开 zip；M3.5：句柄复用优化）
    std::vector<uint8_t> file;
    if (!DocxReader::readMedia(srcPath_, me->zipPath, file)) {
        ++st_.decodeErrors;
        return nullptr;
    }

    DecodedImage img;
    bool ok = false;
    if (file.size() >= 8 && file[0] == 0x89 && file[1] == 0x50)   // PNG
        ok = pngDecode(file, img);
    else if (external_)                                            // JPEG 等
        ok = external_(file, img);
    if (!ok) { ++st_.unsupported; return nullptr; }

    // 下采样到目标尺寸后缓存
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
