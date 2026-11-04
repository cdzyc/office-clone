// oc::DocxWriter — Document → .docx（与 DocxReader 对偶）
// 策略：
//  1. 全量重写 word/document.xml + rels + Content_Types（增量保存 M3）；
//  2. 媒体从源 docx 原样搬运（字节级保真，不重编码）；
//  3. 字体名原样输出（ADR A8：缺失字体绝不改写）。
#pragma once
#include "Document.h"
#include <string>

namespace oc {

struct DocxWriteResult {
    bool ok = false;
    std::string error;
    uint64_t writtenBytes = 0;
};

class DocxWriter {
public:
    // mediaProvider：relId → 媒体内容（DocxReader::readMedia 读源 zip 实现）。
    // 若返回 false 则跳过该媒体并记入警告。
    using MediaProvider = bool (*)(const std::string& relId,
                                   const std::string& zipPath,
                                   std::vector<uint8_t>& out);

    static DocxWriteResult write(const std::string& path, const Document& doc,
                                 MediaProvider media = nullptr,
                                 std::vector<std::string>* warnings = nullptr);

private:
    static std::string buildDocumentXml(const Document& doc);
    static std::string buildRelsXml(const Document& doc);
    static std::string buildContentTypesXml(const Document& doc);
    static std::string buildRootRelsXml();
    static std::string xmlEscape(const std::string& s);
};

} // namespace oc
