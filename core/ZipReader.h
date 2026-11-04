// oc::ZipReader — 零依赖 ZIP 只读解析（central directory 索引 + 按条目读取）
// 两种模式：
//   open(path)    索引模式（推荐/默认）：文件句柄常驻，只解析中央目录，
//                 read() 按需 fseek 读条目——几百 MB 标书不整体驻内存，取图无 IO 放大。
//                 ⚠ 单线程使用（FILE* 非线程安全；多线程 M3.5 加锁）。
//   openMemory()  内存模式：整包已在内存时用（测试/小文件）。
// 支持经典 ZIP（deflate/stored）；Zip64 在 M3.5 补充（当前明确报错，<2GB 偏移）。
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
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
    ZipReader() = default;
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;             // FILE* 独占
    ZipReader& operator=(const ZipReader&) = delete;

    // 索引模式：句柄常驻，条目按需读取（大文件友好）
    bool open(const std::string& path, std::string* err = nullptr);
    // 内存模式：整包已在内存
    bool openMemory(const std::vector<uint8_t>& data, std::string* err = nullptr);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    const ZipEntry* find(const std::string& name) const;

    // 读取条目原始数据（自动解压 deflate）。maxOut 防炸弹。
    bool read(const ZipEntry& e, std::vector<uint8_t>& out,
              size_t maxOut = 512ull * 1024 * 1024, std::string* err = nullptr) const;

    uint32_t crcCheck(const std::vector<uint8_t>& data) const; // CRC-32（读出后可校验）

private:
    bool parseCentral(std::string* err);
    bool readRaw(const ZipEntry& e, std::vector<uint8_t>& raw, std::string* err) const;
    FILE* fp_ = nullptr;                               // 索引模式句柄
    uint64_t fileSize_ = 0;
    std::vector<uint8_t> data_;                        // 仅内存模式
    std::vector<ZipEntry> entries_;
};

} // namespace oc
