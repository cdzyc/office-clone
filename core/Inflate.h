// oc::Inflate — 零依赖 DEFLATE 解码器（RFC 1951）
// office-clone core. 参考经典 puff 实现，简化为纯 C++20。
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace oc {

// 解压 raw DEFLATE 流。成功时追加到 out 并返回 true。
// maxOut 防炸弹流（标书场景限制单条目 ≤ 512MB）。
bool inflate(const uint8_t* src, size_t srcLen,
             std::vector<uint8_t>& out, size_t maxOut = 512ull * 1024 * 1024);

inline bool inflate(const std::vector<uint8_t>& src,
                    std::vector<uint8_t>& out, size_t maxOut = 512ull * 1024 * 1024) {
    return inflate(src.data(), src.size(), out, maxOut);
}

} // namespace oc
