// oc::ZipWriter — 零依赖 ZIP 写入器（与 ZipReader 对偶）
// 智能压缩：deflate 后若更大自动降级 stored（对已压缩图片友好）。
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace oc {

class ZipWriter {
public:
    ZipWriter() = default;
    ~ZipWriter() { if (fp_) fclose(fp_); }

    bool open(const std::string& path, std::string* err = nullptr);

    // 追加条目（compress=true 时智能选择 deflate/stored）
    bool add(const std::string& name, const std::vector<uint8_t>& data,
             bool compress = true, std::string* err = nullptr);

    bool finalize(std::string* err = nullptr);

private:
    struct CentralRec {
        std::string name;
        uint16_t method = 0;
        uint32_t crc = 0;
        uint64_t compSize = 0, uncompSize = 0;
        uint64_t localOffset = 0;
        uint16_t dosTime = 0, dosDate = 0;
    };
    FILE* fp_ = nullptr;
    std::vector<CentralRec> recs_;
    uint64_t curOffset_ = 0;
};

} // namespace oc
