// oc::deflate — 零依赖 DEFLATE 压缩器（RFC 1951）
// 策略：fixed-Huffman 单块 + 贪心 LZ77（3字节hash链，32KB窗口，最长258）
// 压缩率中等（文本约50%+），100% 标准兼容——由 inflate 与 python zlib 双向对拍保障。
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace oc {

// 压缩 src 为 raw DEFLATE 流（追加到 out）。
// level 预留（当前仅 0=快速贪心）。
bool deflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, int level = 0);

inline bool deflate(const std::vector<uint8_t>& src, std::vector<uint8_t>& out, int level = 0) {
    return deflate(src.data(), src.size(), out, level);
}

} // namespace oc
