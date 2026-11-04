// oc::XmlPull — 轻量 XML 拉式解析器（零依赖）
// 面向 OOXML 流式解析：StartTag / EndTag / Text / CData / 指令与声明跳过。
// 命名空间前缀保留原样（如 w:p）；实体解码支持预定义 + 数字字符引用。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oc {

enum class XmlEvent {
    None,
    StartTag,   // <tag attr="v">
    EndTag,     // </tag> 或自闭合产生的事件
    Text,       // 字符数据（已合并实体；isWhitespace 标记纯空白）
    CData,      // <![CDATA[...]]>
    Decl,       // <?xml ... ?> / <!-- 注释 -->（内容跳过，仍上报类型方便调试）
    Error
};

struct XmlAttr {
    std::string name, value;
};

class XmlPullParser {
public:
    // 输入为 UTF-8 字节流（docx 的 XML 部件）
    void setInput(std::string_view utf8);

    XmlEvent next();                       // 返回下一事件
    XmlEvent type() const { return ev_; }
    const std::string& name() const { return name_; }        // 当前标签名（含前缀）
    const std::vector<XmlAttr>& attrs() const { return attrs_; }
    const std::string& text() const { return text_; }        // Text/CData 内容
    bool isSelfClosing() const { return selfClosing_; }
    bool isWhitespaceText() const { return wsOnly_; }
    std::string error() const { return err_; }

    // 便捷：当前 StartTag 属性查找（不存在返回 nullptr）
    const std::string* attr(const char* qname) const;

    // 便捷：读取当前元素的完整文本内容（<w:t>text</w:t>）
    // 调用前提：type()==StartTag。返回拼接文本直到匹配 EndTag。
    std::string readElementText();

private:
    bool parseTag();
    bool parseText();
    bool decodeEntities(const std::string& in, std::string& out, bool& wsOnly);
    void fail(const std::string& msg);

    std::string_view in_;
    size_t pos_ = 0;
    XmlEvent ev_ = XmlEvent::None;
    std::string name_, text_, err_;
    std::vector<XmlAttr> attrs_;
    bool selfClosing_ = false;
    bool wsOnly_ = false;
    bool done_ = false;
    int depth_ = 0;
};

// UTF-8 工具（docx 文本处理需要）
void appendUtf8(std::string& out, uint32_t cp);

} // namespace oc
