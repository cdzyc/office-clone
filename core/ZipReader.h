// oc::ZipReader — 零依赖 ZIP 只读解析（central directory 索引 + 按条目读取）
// office-clone core. 支持经典 ZIP（deflate/stored）；Zip64 在 M3 补充（当前明确报错）。
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace oc {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;          // 0=stored 8=deflate
    uint32_t crc32 = 0;
    uint64_t compSize = 0;
    uint64_t uncompSize = 0;
    uint64_t localOffset = 0;     // local file header 偏移
    bool isDir = false;
};

// CRC-32（IEEE），ZIP/DocxWriter 共用
uint32_t crc32(const uint8_t* data, size_t n);

class ZipReader {
public:
    // 整文件载入内存（标书场景：几百 MB 可接受；M3 可演进为文件句柄+分段映射）
    bool open(const std::string& path, std::string* err = nullptr);
    bool openMemory(const std::vector<uint8_t>& data, std::string* err = nullptr);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    const ZipEntry* find(const std::string& name) const;

    // 读取条目原始数据（自动解压 deflate）。maxOut 防炸弹。
    bool read(const ZipEntry& e, std::vector<uint8_t>& out,
              size_t maxOut = 512ull * 1024 * 1024, std::string* err = nullptr) const;

    uint32_t crcCheck(const std::vector<uint8_t>& data) const; // CRC-32（读出后可校验）

private:
    bool parseCentral(std::string* err);
    std::vector<uint8_t> data_;
    std::vector<ZipEntry> entries_;
};

} // namespace oc
