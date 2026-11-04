#include "Deflate.h"
#include <cstring>

namespace oc {
namespace {

// ---------- 位写入器 ----------
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    // LSB-first（长度/距离的 extra bits、块头等）
    void bits(uint32_t v, int n) {
        for (int i = 0; i < n; ++i) {
            buf_ |= ((v >> i) & 1u) << cnt_;
            if (++cnt_ == 8) flushByte();
        }
    }
    // MSB-first（Huffman 码：高位先入流）
    void code(uint32_t c, int n) {
        for (int i = n - 1; i >= 0; --i) {
            buf_ |= ((c >> i) & 1u) << cnt_;
            if (++cnt_ == 8) flushByte();
        }
    }
    void align() { while (cnt_ != 0) { buf_ |= 0u << cnt_; if (++cnt_ == 8) flushByte(); } }

private:
    void flushByte() { out_.push_back((uint8_t)buf_); buf_ = 0; cnt_ = 0; }
    std::vector<uint8_t>& out_;
    uint32_t buf_ = 0;
    int cnt_ = 0;
};

// fixed Huffman：literal/length 符号 → (code, bits)
inline void putLit(BitWriter& bw, int sym) {
    if (sym <= 143)       bw.code(0x30u + (uint32_t)sym, 8);            // 00110000..
    else if (sym <= 255)  bw.code(0x190u + (uint32_t)(sym - 144), 9);   // 110010000..
    else if (sym <= 279)  bw.code((uint32_t)(sym - 256), 7);            // 0000000..
    else                  bw.code(0xC0u + (uint32_t)(sym - 280), 8);    // 11000000..
}

// length → (code, extraBits, extraVal)
inline void putLength(BitWriter& bw, int len) {
    static const int base[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    for (int c = 28; c >= 0; --c) {
        if (len >= base[c]) {
            putLit(bw, 257 + c);
            if (extra[c]) bw.bits((uint32_t)(len - base[c]), extra[c]);
            return;
        }
    }
}

// distance → (code 5位 + extra)
inline void putDist(BitWriter& bw, int dist) {
    static const int base[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (int c = 29; c >= 0; --c) {
        if (dist >= base[c]) {
            bw.code((uint32_t)c, 5);                       // fixed: 5位码 = 符号号
            if (extra[c]) bw.bits((uint32_t)(dist - base[c]), extra[c]);
            return;
        }
    }
}

// ---------- LZ77：3字节 hash 链（贪心最长匹配）----------
struct HashChain {
    static constexpr uint32_t kHashBits = 15, kHashSize = 1u << kHashBits, kWin = 32768, kMaxChain = 32;
    static constexpr uint32_t kMinMatch = 3, kMaxMatch = 258;

    const uint8_t* p; size_t n;
    int32_t head[kHashSize];
    int32_t* prev;                    // 大小 = n（每位置一条链）

    explicit HashChain(const uint8_t* src, size_t len) : p(src), n(len) {
        memset(head, -1, sizeof(head));
        prev = new int32_t[n ? n : 1];
        memset(prev, -1, (n ? n : 1) * sizeof(int32_t));
    }
    ~HashChain() { delete[] prev; }

    static uint32_t hash3(const uint8_t* q) {
        uint32_t v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
        return (v * 2654435761u) >> (32 - kHashBits);
    }
    void insert(size_t pos) {
        if (pos + kMinMatch > n) return;
        uint32_t h = hash3(p + pos);
        prev[pos] = head[h];
        head[h] = (int32_t)pos;
    }
    // 返回匹配长度（<3 表示无匹配），*dist 输出距离
    int findMatch(size_t pos, int* dist) const {
        if (pos + kMinMatch > n) return 0;
        uint32_t h = hash3(p + pos);
        int32_t cand = head[h];
        int bestLen = 0, bestDist = 0, chain = 0;
        size_t limit = pos > kWin ? pos - kWin : 0;
        while (cand >= (int32_t)limit && chain++ < (int)kMaxChain) {
            size_t c = (size_t)cand;
            // 快速判断首3字节
            if (p[c] == p[pos] && p[c + 1] == p[pos + 1] && p[c + 2] == p[pos + 2]) {
                size_t maxLen = n - pos;
                if (maxLen > kMaxMatch) maxLen = kMaxMatch;
                size_t l = 0;
                while (l < maxLen && p[c + l] == p[pos + l]) ++l;
                if ((int)l > bestLen) {
                    bestLen = (int)l;
                    bestDist = (int)(pos - c);
                    if (bestLen >= (int)kMaxMatch) break;   // 不可能更长
                }
            }
            cand = prev[c];
        }
        *dist = bestDist;
        return bestLen;
    }
};

} // namespace

bool deflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, int level) {
    (void)level;
    BitWriter bw(out);
    // 块头：BFINAL=1, BTYPE=01(fixed)
    bw.bits(1, 1);
    bw.bits(1, 2);

    if (srcLen == 0) { putLit(bw, 256); bw.align(); return true; }

    HashChain hc(src, srcLen);
    size_t i = 0;
    while (i < srcLen) {
        int dist = 0;
        int len = hc.findMatch(i, &dist);
        if (len >= (int)HashChain::kMinMatch) {
            putLength(bw, len);
            putDist(bw, dist);
            // 匹配区间逐点入链（保证后续可引用）
            for (size_t k = 0; k < (size_t)len; ++k) hc.insert(i + k);
            i += (size_t)len;
        } else {
            putLit(bw, src[i]);
            hc.insert(i);
            ++i;
        }
    }
    putLit(bw, 256);      // end-of-block
    bw.align();
    return true;
}

} // namespace oc
