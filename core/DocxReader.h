// oc::DocxReader — .docx → Document（流式解析 document.xml + 媒体索引）
// 策略：ZIP 条目级懒加载；document.xml 一次性流式扫（几百页正文 XML 几 MB～几十 MB 可接受，
// M3 演进为分段索引）；media 只建索引绝不解码。
#pragma once
#include "Document.h"
#include "ZipReader.h"
#include <string>

namespace oc {

struct DocxReadResult {
    bool ok = false;
    std::string error;
    // 解析告警（不致命）：如不支持的特性
    std::vector<std::string> warnings;
};

class DocxReader {
public:
    // 打开并解析（阻塞；M3 可拆为先 open 索引、再后台解析正文）
    static DocxReadResult read(const std::string& path, Document& doc);

    // 解析后保留 zip（供按需读图/读样式）。use后调用 readMedia。
    static bool readMedia(const std::string& path, const std::string& zipPath,
                          std::vector<uint8_t>& out, std::string* err = nullptr);

private:
    static bool parseDocumentXml(const std::vector<uint8_t>& xml, Document& doc,
                                 const std::string& mediaBase,
                                 std::vector<std::string>& warnings);
    static bool parseRels(const std::vector<uint8_t>& xml, const std::string& base,
                          Document& doc);
};

} // namespace oc
