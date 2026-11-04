#include "Inflate.h"
#include <cstring>

namespace oc {
namespace {

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    // LSB-first 取位（注意：用 cnt_ 判断缓冲空，buf_==0 是合法数据态）
    int bit() {
        if (cnt_ == 0) {
            if (pos_ >= n_) return -1;          // 流结束
            buf_ = p_[pos_++];
            cnt_ = 8;
        }
        int b = buf_ & 1;
        buf_ >>= 1;
        --cnt_;
        return b;
    }
    // 读 bits 位（LSB-first 组合）
    int bits(int bits) {
        int v = 0;
        for (int i = 0; i < bits; ++i) {
            int b = bit();
            if (b < 0) return -1;
            v |= b << i;
        }
        return v;
    }
    // 对齐到字节边界并读取 n 个原始字节（用于 stored 块）
    bool rawBytes(size_t n, std::vector<uint8_t>& out) {
        // 先清掉 bit 缓冲里的剩余位
        if (cnt_ > 0) { buf_ = 0; cnt_ = 0; }
        for (size_t i = 0; i < n; ++i) {
            if (pos_ >= n_) return false;
            out.push_back(p_[pos_++]);
        }
        return true;
    }
    bool exhausted() const { return pos_ >= n_; }

private:
    const uint8_t* p_; size_t n_;
    size_t pos_ = 0;
    unsigned buf_ = 0; int cnt_ = 0;
};

struct Huffman {
    // canonical code: 每长度数量 + 符号表
    short count[16] = {0};
    std::vector<short> symbol;

    // 从 code lengths 构建
    bool build(const uint8_t* lengths, int n) {
        symbol.assign(n, 0);
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (int i = 0; i < n; ++i) count[lengths[i]]++;
        if (count[0] == n) return true;          // 空表（无符号）
        // 校验：过度集检测
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;          // 超集——非法
        }
        // 偏移
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len)
            offs[len + 1] = offs[len] + count[len];
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<short>(i);
        return true;
    }
};

// canonical 霍夫曼解码（puff 风格逐长度扫描）
int decode(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        int b = br.bit();
        if (b < 0) return -1;
        code |= b;
        int cnt = h.count[len];
        if (code - cnt < first)
            return h.symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    return -1;                                   // 超长码——非法
}

// 动态块的码长解码顺序
constexpr int kOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

// length 码附加位表
constexpr int kLenBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
// distance 码附加位表
constexpr int kDistBase[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr int kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool codes(BitReader& br, const Huffman& lencode, const Huffman& distcode,
           std::vector<uint8_t>& out, size_t maxOut) {
    for (;;) {
        int sym = decode(br, lencode);
        if (sym < 0) return false;
        if (sym < 256) {                         // 字面量
            if (out.size() >= maxOut) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;                         // 块结束
        } else {                                 // 长度-距离对
            sym -= 257;
            if (sym >= 29) return false;
            int len = kLenBase[sym];
            int le = kLenExtra[sym];
            int lv = le ? br.bits(le) : 0;
            if (lv < 0) return false;
            len += lv;

            int dsym = decode(br, distcode);
            if (dsym < 0 || dsym >= 30) return false;
            int dist = kDistBase[dsym];
            int de = kDistExtra[dsym];
            int dv = de ? br.bits(de) : 0;
            if (dv < 0) return false;
            dist += dv;

            if ((size_t)dist > out.size()) return false;   // 距离越界（引用窗口之前）
            if (out.size() + (size_t)len > maxOut) return false;
            size_t from = out.size() - (size_t)dist;
            for (int i = 0; i < len; ++i)
                out.push_back(out[from + i]);
        }
    }
}

bool fixedTables(Huffman& lencode, Huffman& distcode) {
    uint8_t lengths[288];
    int i = 0;
    for (; i < 144; ++i) lengths[i] = 8;
    for (; i < 256; ++i) lengths[i] = 9;
    for (; i < 280; ++i) lengths[i] = 7;
    for (; i < 288; ++i) lengths[i] = 8;
    if (!lencode.build(lengths, 288)) return false;
    uint8_t dl[30];
    for (i = 0; i < 30; ++i) dl[i] = 5;
    return distcode.build(dl, 30);
}

bool dynamicTables(BitReader& br, Huffman& lencode, Huffman& distcode) {
    int nlen = br.bits(5);   if (nlen < 0) return false;  nlen += 257;
    int ndist = br.bits(5);  if (ndist < 0) return false; ndist += 1;
    int ncode = br.bits(4);  if (ncode < 0) return false; ncode += 4;
    if (nlen > 286 || ndist > 30) return false;

    uint8_t lengths[320] = {0};
    for (int i = 0; i < ncode; ++i) {
        int v = br.bits(3);
        if (v < 0) return false;
        lengths[kOrder[i]] = static_cast<uint8_t>(v);
    }
    Huffman clen;
    if (!clen.build(lengths, 19)) return false;

    // 解码 lencode/distcode 的码长（含 16/17/18 重复码）
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(br, clen);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
        } else {
            int rep = 0, val = 0;
            if (sym == 16) {
                if (i == 0) return false;
                val = lengths[i - 1];
                int e = br.bits(2); if (e < 0) return false;
                rep = 3 + e;
            } else if (sym == 17) {
                int e = br.bits(3); if (e < 0) return false;
                rep = 3 + e; val = 0;
            } else { // 18
                int e = br.bits(7); if (e < 0) return false;
                rep = 11 + e; val = 0;
            }
            if (i + rep > nlen + ndist) return false;
            while (rep--) lengths[i++] = static_cast<uint8_t>(val);
        }
    }
    // 校验：两个 end-of-block 码必须有且长度合法
    if (lengths[256] == 0) return false;
    if (!lencode.build(lengths, nlen)) return false;
    return distcode.build(lengths + nlen, ndist);
}

} // namespace

bool inflate(const uint8_t* src, size_t srcLen,
             std::vector<uint8_t>& out, size_t maxOut) {
    if (!src && srcLen) return false;
    BitReader br(src, srcLen);
    bool final = false;
    while (!final) {
        int b = br.bit();
        if (b < 0) return false;
        final = b != 0;
        int type = br.bits(2);
        if (type < 0) return false;
        if (type == 0) {                          // stored
            br.rawBytes(0, out);                  // 对齐（丢弃 bit 缓冲）
            std::vector<uint8_t> hdr;             // 注意：不能预分配——rawBytes 是 push_back
            hdr.reserve(4);
            if (!br.rawBytes(4, hdr)) return false;
            size_t len = hdr[0] | (hdr[1] << 8);
            size_t nlen = hdr[2] | (hdr[3] << 8);
            if ((len ^ 0xFFFF) != nlen) return false;   // NLEN 校验
            if (out.size() + len > maxOut) return false;
            if (!br.rawBytes(len, out)) return false;
        } else if (type == 1) {                   // fixed
            Huffman lc, dc;
            if (!fixedTables(lc, dc)) return false;
            if (!codes(br, lc, dc, out, maxOut)) return false;
        } else if (type == 2) {                   // dynamic
            Huffman lc, dc;
            if (!dynamicTables(br, lc, dc)) return false;
            if (!codes(br, lc, dc, out, maxOut)) return false;
        } else {
            return false;                         // type 3 保留
        }
    }
    return true;
}

} // namespace oc
