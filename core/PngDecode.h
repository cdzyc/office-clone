// oc::PngDecode — 零依赖 PNG 解码器（RFC 2083 关键子集）
// 支持：8-bit 灰度/灰度A/RGB/RGBA，非隔行；输出统一 RGBA8。
// 不支持（明确报错）：索引色、16-bit、隔行、tRNS —— 外部解码器可经 ImageCache 注入。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oc {

struct DecodedImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;        // w*h*4
    size_t bytes() const { return rgba.size() + 16; }
};

// 解码 PNG 字节流 → RGBA。失败返回 false（err 可空）。
bool pngDecode(const uint8_t* src, size_t n, DecodedImage& out, std::string* err = nullptr);
inline bool pngDecode(const std::vector<uint8_t>& src, DecodedImage& out, std::string* err = nullptr) {
    return pngDecode(src.data(), src.size(), out, err);
}

// 盒式下采样（任意比例）：生成不超过 targetW × targetH 的缩略图。
// upsample 不做（宽高钳制）。
DecodedImage downsample(const DecodedImage& img, uint32_t targetW, uint32_t targetH);

} // namespace oc
