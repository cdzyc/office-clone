#include "ZipReader.h"
#include "Inflate.h"
#include <cstdio>
#include <cstring>

namespace oc {
namespace {

constexpr uint32_t kEocdSig   = 0x06054b50;
constexpr uint32_t kCenSig    = 0x02014b50;
constexpr uint32_t kLocalSig  = 0x04034b50;
constexpr uint32_t kZip64Eocd = 0x06064b50;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

// CRC-32 (IEEE 0xEDB88320)，查表法
struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
const CrcTable kCrc;

} // namespace

uint32_t crc32(const uint8_t* data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = kCrc.t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

ZipReader::~ZipReader() {
    if (fp_) fclose(fp_);
}

// ---- 内部：读 [off, off+len) 字节（索引模式 fseek；内存模式切片）----
static bool readRange(FILE* fp, const std::vector<uint8_t>& mem, uint64_t total,
                      uint64_t off, size_t len, std::vector<uint8_t>& buf) {
    if (off + len > total) return false;
    buf.resize(len ? len : 1);
    if (fp) {
        if (fseek(fp, (long)off, SEEK_SET) != 0) return false;   // <2GB（Zip64 之外）
        return fread(buf.data(), 1, len, fp) == len;
    }
    if (len) memcpy(buf.data(), mem.data() + (size_t)off, len);
    return true;
}

bool ZipReader::open(const std::string& path, std::string* err) {
    if (fp_) { fclose(fp_); fp_ = nullptr; }
    data_.clear(); entries_.clear(); fileSize_ = 0;
    fp_ = fopen(path.c_str(), "rb");
    if (!fp_) { if (err) *err = "cannot open file: " + path; return false; }
    if (fseek(fp_, 0, SEEK_END) != 0) { if (err) *err = "seek failed"; return false; }
    long sz = ftell(fp_);
    if (sz < 0) { if (err) *err = "ftell failed"; return false; }
    fileSize_ = (uint64_t)sz;
    return parseCentral(err);
}

bool ZipReader::openMemory(const std::vector<uint8_t>& data, std::string* err) {
    if (fp_) { fclose(fp_); fp_ = nullptr; }
    data_ = data;
    fileSize_ = data_.size();
    return parseCentral(err);
}

bool ZipReader::parseCentral(std::string* err) {
    entries_.clear();
    const uint64_t n = fp_ ? fileSize_ : (uint64_t)data_.size();
    if (n < 22) { if (err) *err = "not a zip (too small)"; return false; }

    // 尾部读 EOCD 搜索窗（EOCD 距尾 ≤ 22 + 注释 ≤ 22+65535）
    size_t scanLen = (size_t)std::min<uint64_t>(n, 22 + 65535);
    std::vector<uint8_t> tail;
    if (!readRange(fp_, data_, n, n - scanLen, scanLen, tail)) {
        if (err) *err = "read tail failed";
        return false;
    }
    size_t eocdInTail = (size_t)-1;
    for (size_t i = scanLen - 22 + 1; i-- > 0; ) {
        if (rd32(&tail[i]) == kEocdSig) { eocdInTail = i; break; }
    }
    if (eocdInTail == (size_t)-1) { if (err) *err = "EOCD not found"; return false; }
    const uint8_t* E = tail.data() + eocdInTail;
    uint64_t eocd = (n - scanLen) + eocdInTail;        // 绝对偏移

    uint16_t diskNo  = rd16(E + 4);      // 本盘号
    uint16_t cenDisk = rd16(E + 6);      // central 目录起始盘号
    uint16_t nDisk   = rd16(E + 8);      // 本盘条目数
    uint16_t nTotal  = rd16(E + 10);     // 总条目数
    uint32_t cenSize = rd32(E + 12);
    uint32_t cenOff  = rd32(E + 16);

    if (diskNo != 0 || cenDisk != 0 || nDisk != nTotal) {
        if (err) *err = "multi-disk zip not supported";
        return false;
    }
    if (nTotal == 0xFFFF || cenSize == 0xFFFFFFFF || cenOff == 0xFFFFFFFF) {
        if (err) *err = "Zip64 not supported yet (planned M3.5)";
        return false;
    }
    // Zip64 locator 探测（EOCD 前 20 字节）
    if (eocd >= 20) {
        std::vector<uint8_t> loc;
        if (readRange(fp_, data_, n, eocd - 20, 4, loc) && rd32(loc.data()) == 0x07064b50) {
            if (err) *err = "Zip64 locator present: not supported yet (planned M3.5)";
            return false;
        }
    }
    (void)kZip64Eocd;

    if ((uint64_t)cenOff + cenSize > n) { if (err) *err = "central dir out of range"; return false; }

    std::vector<uint8_t> cen;
    if (!readRange(fp_, data_, n, cenOff, cenSize, cen)) {
        if (err) *err = "read central failed";
        return false;
    }

    size_t p = 0;
    entries_.reserve(nTotal);
    for (uint16_t i = 0; i < nTotal; ++i) {
        if (p + 46 > cenSize || rd32(&cen[p]) != kCenSig) {
            if (err) *err = "bad central entry";
            return false;
        }
        ZipEntry e;
        e.method      = rd16(&cen[p + 10]);
        e.crc32       = rd32(&cen[p + 16]);
        e.compSize    = rd32(&cen[p + 20]);
        e.uncompSize  = rd32(&cen[p + 24]);
        uint16_t nameLen = rd16(&cen[p + 28]);
        uint16_t extraLen = rd16(&cen[p + 30]);
        uint16_t commentLen = rd16(&cen[p + 32]);
        e.localOffset = rd32(&cen[p + 42]);
        if (p + 46 + nameLen > cenSize) { if (err) *err = "bad name"; return false; }
        e.name.assign(reinterpret_cast<const char*>(&cen[p + 46]), nameLen);
        e.isDir = !e.name.empty() && e.name.back() == '/';
        entries_.push_back(std::move(e));
        p += 46 + (size_t)nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipEntry* ZipReader::find(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

// 读取条目压缩原始数据（local header 校验 + 定位）
bool ZipReader::readRaw(const ZipEntry& e, std::vector<uint8_t>& raw, std::string* err) const {
    const uint64_t n = fp_ ? fileSize_ : (uint64_t)data_.size();
    std::vector<uint8_t> lh;
    if (!readRange(fp_, data_, n, e.localOffset, 30, lh) || rd32(lh.data()) != kLocalSig) {
        if (err) *err = "bad local header: " + e.name;
        return false;
    }
    uint16_t nameLen  = rd16(lh.data() + 26);
    uint16_t extraLen = rd16(lh.data() + 28);
    uint64_t dataOff  = e.localOffset + 30 + nameLen + extraLen;
    if (!readRange(fp_, data_, n, dataOff, (size_t)e.compSize, raw)) {
        if (err) *err = "entry data out of range: " + e.name;
        return false;
    }
    return true;
}

bool ZipReader::read(const ZipEntry& e, std::vector<uint8_t>& out,
                     size_t maxOut, std::string* err) const {
    out.clear();
    std::vector<uint8_t> raw;
    if (!readRaw(e, raw, err)) return false;

    if (e.method == 0) {                          // stored
        if (e.uncompSize != e.compSize) { if (err) *err = "stored size mismatch"; return false; }
        if (e.uncompSize > maxOut) { if (err) *err = "entry too large"; return false; }
        out = std::move(raw);
    } else if (e.method == 8) {                   // deflate
        if (!oc::inflate(raw.data(), raw.size(), out, maxOut)) {
            if (err) *err = "inflate failed: " + e.name;
            return false;
        }
        if (e.uncompSize != 0 && out.size() != e.uncompSize) {
            if (err) *err = "size mismatch after inflate";
            return false;
        }
    } else {
        if (err) *err = "unsupported compression method";
        return false;
    }
    return true;
}

uint32_t ZipReader::crcCheck(const std::vector<uint8_t>& data) const {
    return oc::crc32(data.data(), data.size());
}

} // namespace oc
