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

bool ZipReader::open(const std::string& path, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open file: " + path; return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); if (err) *err = "ftell failed"; return false; }
    data_.resize((size_t)sz);
    size_t got = fread(data_.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { if (err) *err = "short read"; return false; }
    return parseCentral(err);
}

bool ZipReader::openMemory(const std::vector<uint8_t>& data, std::string* err) {
    data_ = data;
    return parseCentral(err);
}

bool ZipReader::parseCentral(std::string* err) {
    entries_.clear();
    const size_t n = data_.size();
    if (n < 22) { if (err) *err = "not a zip (too small)"; return false; }

    // 从尾部找 EOCD（后面可能跟注释，最多 64KB）
    size_t eocd = (size_t)-1;
    size_t scanFrom = n >= 22 + 65535 ? n - 65535 - 22 : 0;
    for (size_t i = n - 22 + 1; i-- > scanFrom; ) {
        if (rd32(&data_[i]) == kEocdSig) { eocd = i; break; }
    }
    if (eocd == (size_t)-1) { if (err) *err = "EOCD not found"; return false; }

    uint16_t diskNo  = rd16(&data_[eocd + 4]);
    uint16_t cenDisk = rd16(&data_[eocd + 6]);
    uint16_t nDisk   = rd16(&data_[eocd + 8]);
    uint16_t nTotal  = rd16(&data_[eocd + 10]);
    uint32_t cenSize = rd32(&data_[eocd + 12]);
    uint32_t cenOff  = rd32(&data_[eocd + 16]);
    (void)diskNo; (void)cenDisk;

    if (nDisk != nTotal) {
        if (err) *err = "multi-disk zip not supported";
        return false;
    }
    if (nTotal == 0xFFFF || cenSize == 0xFFFFFFFF || cenOff == 0xFFFFFFFF) {
        if (err) *err = "Zip64 not supported yet (planned M3)";
        return false;
    }
    // Zip64 EOCD 探测（位于 EOCD 前）——明确报错而不是误读
    if (eocd >= 20 && rd32(&data_[eocd - 20]) == 0x07064b50) {
        if (err) *err = "Zip64 locator present: not supported yet (planned M3)";
        return false;
    }
    (void)kZip64Eocd;

    if ((uint64_t)cenOff + cenSize > n) { if (err) *err = "central dir out of range"; return false; }

    size_t p = cenOff;
    entries_.reserve(nTotal);
    for (uint16_t i = 0; i < nTotal; ++i) {
        if (p + 46 > n || rd32(&data_[p]) != kCenSig) {
            if (err) *err = "bad central entry";
            return false;
        }
        ZipEntry e;
        e.method      = rd16(&data_[p + 10]);
        e.crc32       = rd32(&data_[p + 16]);
        e.compSize    = rd32(&data_[p + 20]);
        e.uncompSize  = rd32(&data_[p + 24]);
        uint16_t nameLen = rd16(&data_[p + 28]);
        uint16_t extraLen = rd16(&data_[p + 30]);
        uint16_t commentLen = rd16(&data_[p + 32]);
        e.localOffset = rd32(&data_[p + 42]);
        if (p + 46 + nameLen > n) { if (err) *err = "bad name"; return false; }
        e.name.assign(reinterpret_cast<const char*>(&data_[p + 46]), nameLen);
        e.isDir = !e.name.empty() && e.name.back() == '/';
        // UTF-8 标志（bit 11）——name 按字节保留，UTF-8 由上层处理
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

bool ZipReader::read(const ZipEntry& e, std::vector<uint8_t>& out,
                     size_t maxOut, std::string* err) const {
    out.clear();
    const size_t n = data_.size();
    if (e.localOffset + 30 > n || rd32(&data_[(size_t)e.localOffset]) != kLocalSig) {
        if (err) *err = "bad local header: " + e.name;
        return false;
    }
    uint16_t nameLen  = rd16(&data_[(size_t)e.localOffset + 26]);
    uint16_t extraLen = rd16(&data_[(size_t)e.localOffset + 28]);
    uint64_t dataOff  = e.localOffset + 30 + nameLen + extraLen;
    if (dataOff + e.compSize > n) { if (err) *err = "entry data out of range"; return false; }

    if (e.method == 0) {                          // stored
        if (e.uncompSize != e.compSize) { if (err) *err = "stored size mismatch"; return false; }
        if (e.uncompSize > maxOut) { if (err) *err = "entry too large"; return false; }
        out.assign(data_.begin() + (ptrdiff_t)dataOff,
                   data_.begin() + (ptrdiff_t)(dataOff + e.compSize));
    } else if (e.method == 8) {                   // deflate
        if (!oc::inflate(&data_[(size_t)dataOff], (size_t)e.compSize, out, maxOut)) {
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
