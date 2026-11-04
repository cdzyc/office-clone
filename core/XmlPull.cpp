#include "XmlPull.h"
#include <cctype>

namespace oc {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

void XmlPullParser::setInput(std::string_view utf8) {
    in_ = utf8; pos_ = 0; ev_ = XmlEvent::None; err_.clear();
    attrs_.clear(); name_.clear(); text_.clear();
    selfClosing_ = wsOnly_ = done_ = false; depth_ = 0;
}

void XmlPullParser::fail(const std::string& msg) {
    if (err_.empty()) err_ = msg + " @offset " + std::to_string(pos_);
    ev_ = XmlEvent::Error;
}

bool XmlPullParser::decodeEntities(const std::string& in, std::string& out, bool& wsOnly) {
    out.clear(); wsOnly = true;
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '&') {
            size_t semi = in.find(';', i + 1);
            if (semi == std::string::npos || semi - i > 12) { fail("bad entity"); return false; }
            std::string ent = in.substr(i + 1, semi - i - 1);
            i = semi + 1;
            if (ent == "amp") out += '&';
            else if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                uint32_t cp = 0;
                bool ok = true;
                if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                    for (size_t k = 2; k < ent.size(); ++k) {
                        char c = ent[k]; int v;
                        if (c >= '0' && c <= '9') v = c - '0';
                        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                        else { ok = false; break; }
                        cp = cp * 16 + (uint32_t)v;
                    }
                } else {
                    for (size_t k = 1; k < ent.size(); ++k) {
                        if (ent[k] < '0' || ent[k] > '9') { ok = false; break; }
                        cp = cp * 10 + (uint32_t)(ent[k] - '0');
                    }
                }
                if (!ok || cp == 0) { fail("bad char ref"); return false; }
                appendUtf8(out, cp);
            } else {
                // OOXML 不常用命名实体——保守报错（可按需扩展）
                fail("unknown entity: &" + ent + ";");
                return false;
            }
            if (out.back() != ' ' && out.back() != '\t' && out.back() != '\n' && out.back() != '\r')
                wsOnly = false;
        } else {
            char c = in[i++];
            out += c;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') wsOnly = false;
        }
    }
    return true;
}

bool XmlPullParser::parseText() {
    size_t start = pos_;
    // 文本直到 '<'
    while (pos_ < in_.size() && in_[pos_] != '<') ++pos_;
    std::string raw(in_.substr(start, pos_ - start));
    if (!decodeEntities(raw, text_, wsOnly_)) return false;
    ev_ = XmlEvent::Text;
    return true;
}

bool XmlPullParser::parseTag() {
    // pos_ 指向 '<' 之后
    if (pos_ >= in_.size()) { fail("EOF in tag"); return false; }
    char c = in_[pos_];

    // </end>
    if (c == '/') {
        ++pos_;
        size_t start = pos_;
        while (pos_ < in_.size() && in_[pos_] != '>') ++pos_;
        if (pos_ >= in_.size()) { fail("EOF in end tag"); return false; }
        name_.assign(in_.substr(start, pos_ - start));
        ++pos_;
        // 去尾空白
        while (!name_.empty() && isspace((unsigned char)name_.back())) name_.pop_back();
        attrs_.clear(); text_.clear();
        selfClosing_ = false;
        ev_ = XmlEvent::EndTag;
        --depth_;
        return true;
    }

    // <!-- 注释 --> / <? ... ?> / <![CDATA[ ... ]]> / <!DOCTYPE ...>
    // 注意：pos_ 已越过 '<'，比较串不带 '<' 前缀
    if (c == '!' || c == '?') {
        if (in_.compare(pos_, 8, "![CDATA[") == 0) {
            size_t end = in_.find("]]>", pos_ + 8);
            if (end == std::string_view::npos) { fail("unterminated CDATA"); return false; }
            text_.assign(in_.substr(pos_ + 8, end - (pos_ + 8)));
            pos_ = end + 3;
            attrs_.clear(); name_.clear();
            wsOnly_ = false;
            ev_ = XmlEvent::CData;
            return true;
        }
        // 注释：<!-- ... -->
        if (in_.compare(pos_, 3, "!--") == 0) {
            size_t end = in_.find("-->", pos_ + 3);
            if (end == std::string_view::npos) { fail("unterminated comment"); return false; }
            pos_ = end + 3;
            ev_ = XmlEvent::Decl; name_ = "#comment"; text_.clear(); attrs_.clear();
            return true;
        }
        // 注释：<!-- ... -->
        if (in_.compare(pos_, 4, "<!--") == 0) {
            size_t end = in_.find("-->", pos_ + 4);
            if (end == std::string_view::npos) { fail("unterminated comment"); return false; }
            pos_ = end + 3;
            ev_ = XmlEvent::Decl; name_ = "#comment"; text_.clear(); attrs_.clear();
            return true;
        }
        // <? ... ?> 或 <!DOCTYPE ...>（简化：DOCTYPE 找匹配 '>'，容忍内部子集）
        if (in_[pos_] == '?') {
            size_t end = in_.find("?>", pos_ + 2);
            if (end == std::string_view::npos) { fail("unterminated PI"); return false; }
            pos_ = end + 2;
        } else {
            size_t end = in_.find('>', pos_);
            if (end == std::string_view::npos) { fail("unterminated decl"); return false; }
            pos_ = end + 1;
        }
        ev_ = XmlEvent::Decl; name_ = "#decl"; text_.clear(); attrs_.clear();
        return true;
    }

    // <tag attr="v" ...> 或 <tag/>
    size_t start = pos_;
    while (pos_ < in_.size() &&
           (isalnum((unsigned char)in_[pos_]) || in_[pos_] == '_' ||
            in_[pos_] == '-' || in_[pos_] == '.' || in_[pos_] == ':'))
        ++pos_;
    if (pos_ == start) { fail("bad tag name"); return false; }
    name_.assign(in_.substr(start, pos_ - start));
    attrs_.clear(); selfClosing_ = false;

    // 属性区
    for (;;) {
        while (pos_ < in_.size() && isspace((unsigned char)in_[pos_])) ++pos_;
        if (pos_ >= in_.size()) { fail("EOF in attrs"); return false; }
        if (in_[pos_] == '>') { ++pos_; ++depth_; ev_ = XmlEvent::StartTag; return true; }
        if (in_[pos_] == '/') {
            if (pos_ + 1 >= in_.size() || in_[pos_ + 1] != '>') { fail("bad self-close"); return false; }
            pos_ += 2; selfClosing_ = true;
            ev_ = XmlEvent::StartTag;      // 自闭合：上报 StartTag，下次 next() 跳过 EndTag
            return true;
        }
        // 属性名
        size_t ns = pos_;
        while (pos_ < in_.size() && in_[pos_] != '=' && !isspace((unsigned char)in_[pos_]) &&
               in_[pos_] != '>' && in_[pos_] != '/')
            ++pos_;
        if (pos_ == ns) { fail("bad attr name"); return false; }
        XmlAttr a;
        a.name.assign(in_.substr(ns, pos_ - ns));
        while (pos_ < in_.size() && isspace((unsigned char)in_[pos_])) ++pos_;
        if (pos_ >= in_.size() || in_[pos_] != '=') { fail("attr missing '='"); return false; }
        ++pos_;
        while (pos_ < in_.size() && isspace((unsigned char)in_[pos_])) ++pos_;
        if (pos_ >= in_.size() || (in_[pos_] != '"' && in_[pos_] != '\'')) { fail("attr quote"); return false; }
        char q = in_[pos_++];
        size_t vs = pos_;
        while (pos_ < in_.size() && in_[pos_] != q) ++pos_;
        if (pos_ >= in_.size()) { fail("unterminated attr"); return false; }
        std::string rawVal(in_.substr(vs, pos_ - vs));
        ++pos_;
        bool wsDummy;
        if (!decodeEntities(rawVal, a.value, wsDummy)) return false;
        attrs_.push_back(std::move(a));
    }
}

XmlEvent XmlPullParser::next() {
    if (done_ || ev_ == XmlEvent::Error) return ev_;

    // 自闭合标签：StartTag 之后补一个 EndTag（不消费输入）
    if (ev_ == XmlEvent::StartTag && selfClosing_) {
        ev_ = XmlEvent::EndTag;
        selfClosing_ = false;
        return ev_;
    }

    // 跳过根元素之后的尾部空白
    while (pos_ < in_.size() && isspace((unsigned char)in_[pos_]) && depth_ == 0) {
        // 仅在文档级跳过；元素内文本交由 parseText
        size_t s = pos_;
        while (pos_ < in_.size() && isspace((unsigned char)in_[pos_])) ++pos_;
        if (pos_ < in_.size() && in_[pos_] == '<') continue;
        pos_ = s;
        break;
    }

    if (pos_ >= in_.size()) { done_ = true; ev_ = XmlEvent::None; return ev_; }

    if (in_[pos_] == '<') {
        ++pos_;
        return parseTag() ? ev_ : ev_;
    }
    if (depth_ == 0) { fail("text at document level"); return ev_; }
    return parseText() ? ev_ : ev_;
}

const std::string* XmlPullParser::attr(const char* qname) const {
    for (const auto& a : attrs_)
        if (a.name == qname) return &a.value;
    return nullptr;
}

std::string XmlPullParser::readElementText() {
    std::string acc;
    // 仅当当前是 StartTag 且非自闭合
    if (ev_ == XmlEvent::StartTag && !selfClosing_) {
        while (next() != XmlEvent::None) {
            if (type() == XmlEvent::Text || type() == XmlEvent::CData) acc += text_;
            else if (type() == XmlEvent::StartTag) { fail("child in element text"); break; }
            else if (type() == XmlEvent::EndTag) break;
        }
    }
    return acc;
}

} // namespace oc
