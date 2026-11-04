#!/usr/bin/env python3
"""互操作验证：用 python 标准 zipfile + XML 解析器严格校验 C++ DocxWriter 产物。
模拟第三方实现（LibreOffice/Word）读取我们保存的 .docx。"""
import sys
import zipfile
import xml.etree.ElementTree as ET
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

failures = []
def check(name, cond):
    print(("PASS" if cond else "FAIL"), name)
    if not cond:
        failures.append(name)

for fname in ["roundtrip_plain.docx", "roundtrip_edited.docx"]:
    path = os.path.join(DATA, fname)
    check(f"{fname}: exists", os.path.exists(path))
    if not os.path.exists(path):
        continue
    # 1) 标准 zipfile 可打开且 CRC 全部正确
    z = zipfile.ZipFile(path)
    bad = z.testzip()
    check(f"{fname}: zip CRC clean", bad is None)
    names = z.namelist()
    check(f"{fname}: has document.xml", "word/document.xml" in names)
    check(f"{fname}: has rels", "word/_rels/document.xml.rels" in names)
    check(f"{fname}: has content types", "[Content_Types].xml" in names)
    check(f"{fname}: has 2 media", sum(1 for n in names if n.startswith("word/media/")) == 2)
    # 2) document.xml 是合法 XML
    doc_xml = z.read("word/document.xml")
    try:
        root = ET.fromstring(doc_xml)
        check(f"{fname}: XML parse ok", True)
    except ET.ParseError as e:
        check(f"{fname}: XML parse ok ({e})", False)
        continue
    # 3) 内容断言
    text = "".join(t.text or "" for t in root.iter(
        "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}t"))
    if fname == "roundtrip_edited.docx":
        check(f"{fname}: edited text persisted", "【修订版】" in text)
        check(f"{fname}: original text kept", "投标文件" in text and "表格文字" not in text)
    else:
        check(f"{fname}: title text", "XX项目投标文件 & 技术方案" in text)
    # 4) 媒体字节保真
    src = zipfile.ZipFile(os.path.join(DATA, "mini_bid.docx"))
    for m in ["word/media/img1.png", "word/media/img2.jpg"]:
        check(f"{fname}: {m} byte-identical",
              z.read(m) == src.read(m))
    # 5) deflate 方法有效性（document.xml 应为压缩条目）
    info = z.getinfo("word/document.xml")
    check(f"{fname}: document.xml compressed", info.compress_type == zipfile.ZIP_DEFLATED)

# 6) w_test.zip 互操作
zp = os.path.join(DATA, "w_test.zip")
if os.path.exists(zp):
    z = zipfile.ZipFile(zp)
    check("w_test.zip: CRC clean", z.testzip() is None)
    check("w_test.zip: text content",
          z.read("text/compressed.txt").decode("utf-8").endswith("标书内容与office-clone混合数据"))
    check("w_test.zip: raw content size", len(z.read("raw.bin")) == 30000)

print()
if failures:
    print(f"=== INTEROP: {len(failures)} FAILED ===")
    sys.exit(1)
print("=== INTEROP: ALL PASSED ===")
