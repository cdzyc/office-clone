#include "ZipWriter.h"
#include "Deflate.h"
#include "ZipReader.h"      // oc::crc32
#include <ctime>
#include <cstring>

namespace oc {
namespace {

void wr16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)(v >> 8));
}
void wr32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)((v >> 8) & 0xFF));
    b.push_back((uint8_t)((v >> 16) & 0xFF));
    b.push_back((uint8_t)((v >> 24) & 0xFF));
}

void dosDateTime(uint16_t* t, uint16_t* d) {
    std::time_t now = std::time(nullptr);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    uint16_t year = (uint16_t)(tmv.tm_year + 1900);
    if (year < 1980) year = 1980;                       // DOS 起始
    *d = (uint16_t)(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    *t = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
}

} // namespace

bool ZipWriter::open(const std::string& path, std::string* err) {
    fp_ = fopen(path.c_str(), "wb");
    if (!fp_) { if (err) *err = "cannot create zip: " + path; return false; }
    recs_.clear();
    curOffset_ = 0;
    return true;
}

bool ZipWriter::add(const std::string& name, const std::vector<uint8_t>& data,
                    bool compress, std::string* err) {
    if (!fp_) { if (err) *err = "zip not open"; return false; }

    // 压缩尝试：仅当数据足够大才值得（极小数据 deflate 开销占比高）
    std::vector<uint8_t> comp;
    uint16_t method = 0;                                 // stored
    if (compress && data.size() > 64) {
        if (!oc::deflate(data, comp)) { if (err) *err = "deflate failed"; return false; }
        if (comp.size() < data.size()) method = 8;       // 更小才用；否则 stored
        else comp.clear();
    }

    CentralRec rec;
    rec.name = name;
    rec.method = method;
    rec.uncompSize = data.size();
    rec.compSize = method == 8 ? comp.size() : data.size();
    rec.crc = oc::crc32(data.data(), data.size());
    rec.localOffset = curOffset_;
    dosDateTime(&rec.dosTime, &rec.dosDate);

    // ---- local file header + 数据 ----
    std::vector<uint8_t> lh;
    wr32(lh, 0x04034b50);
    wr16(lh, 20);                                        // version needed
    wr16(lh, 0);                                         // flags
    wr16(lh, method);
    wr16(lh, rec.dosTime); wr16(lh, rec.dosDate);
    wr32(lh, rec.crc);
    wr32(lh, (uint32_t)rec.compSize);
    wr32(lh, (uint32_t)rec.uncompSize);
    wr16(lh, (uint16_t)name.size());
    wr16(lh, 0);                                         // extra len
    lh.insert(lh.end(), name.begin(), name.end());

    if (fwrite(lh.data(), 1, lh.size(), fp_) != lh.size()) {
        if (err) *err = "write local header failed";
        return false;
    }
    const std::vector<uint8_t>& payload = (method == 8) ? comp : data;
    if (!payload.empty() &&
        fwrite(payload.data(), 1, payload.size(), fp_) != payload.size()) {
        if (err) *err = "write entry data failed";
        return false;
    }
    curOffset_ += lh.size() + payload.size();

    recs_.push_back(std::move(rec));
    return true;
}

bool ZipWriter::finalize(std::string* err) {
    if (!fp_) { if (err) *err = "zip not open"; return false; }

    uint64_t cenStart = curOffset_;
    uint64_t cenSize = 0;
    for (const auto& r : recs_) {
        std::vector<uint8_t> ch;
        wr32(ch, 0x02014b50);
        wr16(ch, 20);                                    // version made by
        wr16(ch, 20);                                    // version needed
        wr16(ch, 0);                                     // flags
        wr16(ch, r.method);
        wr16(ch, r.dosTime); wr16(ch, r.dosDate);
        wr32(ch, r.crc);
        wr32(ch, (uint32_t)r.compSize);
        wr32(ch, (uint32_t)r.uncompSize);
        wr16(ch, (uint16_t)r.name.size());
        wr16(ch, 0); wr16(ch, 0);                        // extra, comment
        wr16(ch, 0); wr16(ch, 0);                        // disk, internal attrs
        wr32(ch, 0);                                     // external attrs
        wr32(ch, (uint32_t)r.localOffset);
        ch.insert(ch.end(), r.name.begin(), r.name.end());

        if (fwrite(ch.data(), 1, ch.size(), fp_) != ch.size()) {
            if (err) *err = "write central failed";
            return false;
        }
        cenSize += ch.size();
    }

    // EOCD
    std::vector<uint8_t> eocd;
    wr32(eocd, 0x06054b50);
    wr16(eocd, 0); wr16(eocd, 0);                        // disk numbers
    wr16(eocd, (uint16_t)recs_.size());
    wr16(eocd, (uint16_t)recs_.size());
    wr32(eocd, (uint32_t)cenSize);
    wr32(eocd, (uint32_t)cenStart);
    wr16(eocd, 0);                                       // comment len
    if (fwrite(eocd.data(), 1, eocd.size(), fp_) != eocd.size()) {
        if (err) *err = "write eocd failed";
        return false;
    }

    fclose(fp_);
    fp_ = nullptr;
    return true;
}

} // namespace oc
