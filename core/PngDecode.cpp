#include "PngDecode.h"
#include "Inflate.h"
#include "ZipReader.h"      // oc::crc32 校验
#include <cstring>

namespace oc {
namespace {

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

} // namespace

bool pngDecode(const uint8_t* src, size_t n, DecodedImage& out, std::string* err) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (n < 8 || std::memcmp(src, "\x89PNG\r\n\x1a\n", 8) != 0)
        return fail("not a png");

    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int bitDepth = 0, colorType = -1, interlace = 0;
    std::vector<uint8_t> idat;

    while (pos + 12 <= n) {
        uint32_t len = be32(src + pos);
        const uint8_t* type = src + pos + 4;
        const uint8_t* data = src + pos + 8;
        if (pos + 12 + (size_t)len > n) return fail("chunk overflow");
        // CRC 校验（type+data）
        if (oc::crc32(type, len + 4) != be32(data + len))
            return fail("chunk crc mismatch");

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) return fail("bad IHDR");
            w = be32(data); h = be32(data + 4);
            bitDepth = data[8]; colorType = data[9];
            interlace = data[12];
            if (data[10] != 0 || data[11] != 0)
                return fail("unsupported compression/filter method");
        } else if (std::memcmp(type, "PLTE", 4) == 0 || std::memcmp(type, "tRNS", 4) == 0) {
            return fail("palette/alpha-key not supported (inject external decoder)");
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    if (!w || !h || colorType < 0) return fail("missing IHDR");
    if (bitDepth != 8) return fail("unsupported bit depth (need 8)");
    if (interlace != 0) return fail("interlaced not supported");
    int channels = colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 4 ? 2
                 : colorType == 6 ? 4 : -1;
    if (channels < 0) return fail("unsupported color type");
    if (w > 30000 || h > 30000) return fail("image too large");

    // inflate 扫描线（PNG IDAT 是 zlib 流：RFC1950 头 + raw deflate + adler32）
    std::vector<uint8_t> raw;
    size_t stride = (size_t)w * channels;
    if (idat.size() < 6 || (idat[0] & 0x0F) != 8 || ((idat[0] << 8) | idat[1]) % 31 != 0)
        return fail("bad zlib header in IDAT");
    if (!oc::inflate(idat.data() + 2, idat.size() - 6, raw, 512ull * 1024 * 1024)
        || raw.size() < (stride + 1) * (size_t)h)
        return fail("idat inflate failed/short");

    // unfilter → 统一 RGBA
    out.width = w; out.height = h;
    out.rgba.assign((size_t)w * h * 4, 0xFF);
    std::vector<uint8_t> prev(stride, 0), cur(stride, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* line = raw.data() + (size_t)y * (stride + 1);
        uint8_t f = line[0];
        const uint8_t* srcRow = line + 1;
        for (size_t x = 0; x < stride; ++x) {
            uint8_t a = x >= (size_t)channels ? cur[x - channels] : 0;
            uint8_t b = prev[x];
            uint8_t c = x >= (size_t)channels ? prev[x - channels] : 0;
            uint8_t v = srcRow[x];
            switch (f) {
            case 0: cur[x] = v; break;
            case 1: cur[x] = (uint8_t)(v + a); break;
            case 2: cur[x] = (uint8_t)(v + b); break;
            case 3: cur[x] = (uint8_t)(v + ((int)a + (int)b) / 2); break;
            case 4: cur[x] = (uint8_t)(v + paeth(a, b, c)); break;
            default: return fail("bad filter");
            }
        }
        uint8_t* dst = out.rgba.data() + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = cur.data() + (size_t)x * channels;
            switch (channels) {
            case 1: dst[x*4] = dst[x*4+1] = dst[x*4+2] = s[0]; dst[x*4+3] = 0xFF; break;
            case 2: dst[x*4] = dst[x*4+1] = dst[x*4+2] = s[0]; dst[x*4+3] = s[1]; break;
            case 3: dst[x*4] = s[0]; dst[x*4+1] = s[1]; dst[x*4+2] = s[2]; dst[x*4+3] = 0xFF; break;
            case 4: dst[x*4] = s[0]; dst[x*4+1] = s[1]; dst[x*4+2] = s[2]; dst[x*4+3] = s[3]; break;
            }
        }
        std::swap(prev, cur);
    }
    return true;
}

DecodedImage downsample(const DecodedImage& img, uint32_t targetW, uint32_t targetH) {
    if (!targetW || !targetH || !img.width || !img.height) return img;
    uint32_t w = targetW >= img.width ? img.width : targetW;
    uint32_t h = targetH >= img.height ? img.height : targetH;
    if (w == img.width && h == img.height) return img;

    DecodedImage out;
    out.width = w; out.height = h;
    out.rgba.assign((size_t)w * h * 4, 0);
    // 盒式：源块 [x0,x1)×[y0,y1) 平均
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t y0 = (uint32_t)((uint64_t)y * img.height / h);
        uint32_t y1 = (uint32_t)(((uint64_t)y + 1) * img.height / h);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > img.height) y1 = img.height;
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t x0 = (uint32_t)((uint64_t)x * img.width / w);
            uint32_t x1 = (uint32_t)(((uint64_t)x + 1) * img.width / w);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > img.width) x1 = img.width;
            uint64_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (uint32_t yy = y0; yy < y1; ++yy) {
                const uint8_t* row = img.rgba.data() + (size_t)yy * img.width * 4;
                for (uint32_t xx = x0; xx < x1; ++xx) {
                    r += row[xx*4]; g += row[xx*4+1]; b += row[xx*4+2]; a += row[xx*4+3];
                    ++cnt;
                }
            }
            uint8_t* d = out.rgba.data() + ((size_t)y * w + x) * 4;
            d[0] = (uint8_t)(r / cnt); d[1] = (uint8_t)(g / cnt);
            d[2] = (uint8_t)(b / cnt); d[3] = (uint8_t)(a / cnt);
        }
    }
    return out;
}

} // namespace oc
