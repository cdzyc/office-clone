#!/usr/bin/env python3
"""生成 core_tests 所需的二进制夹具（在仓库外运行一次即可）：
  data/deflate_sample.deflate / .expected — 随机文本 raw-deflate 压缩流
  data/plain.zip                         — stored + deflate 混合小 zip
  data/mini_bid.docx                     — 迷你标书（段落/表格/图片rels/页面设置）
"""
import os
import random
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
PNGDIR = os.path.join(DATA, "png")
os.makedirs(DATA, exist_ok=True)
os.makedirs(PNGDIR, exist_ok=True)

random.seed(2026)


def _chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)


def make_png(w, h, ct, filt_mode, name=None, out_dir=None):
    """手写 PNG 编码器（8-bit；ct: 0=Gray 2=RGB 4=GrayA 6=RGBA；filt_mode: 0-4 或 -1 混合）
    含真正的 filter 编码（Sub/Up/Average/Paeth）——与 C++ 解码端交叉验证。"""
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]

    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
        if pa <= pb and pa <= pc: return a
        if pb <= pc: return b
        return c

    def orig(x, y):
        """规律像素（字节级通道序列）"""
        if ct == 2:
            return bytes([(x*7+y*3) & 255, (x*13+y*11) & 255, (x*29+y*17) & 255])
        if ct == 6:
            return bytes([(x*5+y*7) & 255, (x*3+y*13) & 255, (x*11+y*19) & 255, (x*17+y*23) & 255])
        if ct == 4:
            return bytes([(x*19+y*31) & 255, (x*7+y*13) & 255])
        return bytes([(x*19+y*31) & 255])

    raw = bytearray()
    for y in range(h):
        f = (y % 5) if filt_mode == -1 else filt_mode
        raw.append(f)
        for x in range(w):
            cur_b = orig(x, y)
            left_b = orig(x-1, y) if x > 0 else b"\x00" * ch
            up_b = orig(x, y-1) if y > 0 else b"\x00" * ch
            upleft_b = orig(x-1, y-1) if (x > 0 and y > 0) else b"\x00" * ch
            if f == 0:
                raw += cur_b
            elif f == 1:      # Sub: -left
                raw += bytes([(cur_b[i] - left_b[i]) & 255 for i in range(ch)])
            elif f == 2:      # Up: -up
                raw += bytes([(cur_b[i] - up_b[i]) & 255 for i in range(ch)])
            elif f == 3:      # Average: -floor((left+up)/2)
                raw += bytes([(cur_b[i] - (left_b[i] + up_b[i]) // 2) & 255 for i in range(ch)])
            else:             # Paeth
                raw += bytes([(cur_b[i] - paeth(left_b[i], up_b[i], upleft_b[i])) & 255 for i in range(ch)])
    hdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", hdr) + _chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + _chunk(b"IEND", b"")
    if name:
        open(os.path.join(out_dir or PNGDIR, name), "wb").write(png)
    return png


# ---- PNG 解码测试集（全部 filter/色彩类型）----
make_png(64, 48, 2, -1, "rgb_mixed.png")
make_png(80, 50, 6, 0, "rgba_none.png")
make_png(33, 21, 0, 4, "gray_paeth.png")
make_png(40, 30, 4, 2, "graya_up.png")
make_png(1200, 900, 2, 1, "big_scan.png")     # 大"扫描件"（性能基准用）
print("png fixtures written")

# ---------- 1) raw deflate 样本 ----------
words = ["标书", "bid", "技术方案", "资质", "扫描件", "报价表", "document", "page"] + \
        ["第%s章" % i for i in range(50)]
plain = "\n".join(" ".join(random.choices(words, k=random.randint(5, 30))) for _ in range(3000))
plain_b = plain.encode("utf-8")
comp = zlib.compressobj(9, zlib.DEFLATED, -15)   # raw deflate
deflate_b = comp.compress(plain_b) + comp.flush()
open(os.path.join(DATA, "deflate_sample.deflate"), "wb").write(deflate_b)
open(os.path.join(DATA, "deflate_sample.expected"), "wb").write(plain_b)
print("deflate sample:", len(deflate_b), "->", len(plain_b), "bytes")

# ---------- 2) plain.zip（stored + deflate）----------
stored_content = b"STORED: office-clone zip reader test, bid document sample." * 4
defl_content = ("DEFLATED: 中文内容测试 entity chars: <>&\"' line\n" * 500).encode("utf-8")
with zipfile.ZipFile(os.path.join(DATA, "plain.zip"), "w") as z:
    zi = zipfile.ZipInfo("stored.txt")
    zi.compress_type = zipfile.ZIP_STORED
    z.writestr(zi, stored_content)
    zi = zipfile.ZipInfo("deflated.txt")
    zi.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(zi, defl_content)
print("plain.zip written")

# ---------- 3) mini_bid.docx 迷你标书 ----------

def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))

img1 = make_png(64, 48, 2, -1)                            # 真 PNG（ImageCache 端到端用）
img2 = bytes(random.randrange(256) for _ in range(2048))  # 假 JPG（unsupported 路径）

document_xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
            xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
            xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
            xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">
<w:body>
  <w:p><w:pPr><w:pStyle w:val="Title"/><w:jc w:val="center"/></w:pPr>
    <w:r><w:rPr><w:rFonts w:ascii="Times New Roman" w:eastAsia="仿宋_GB2312"/><w:b/><w:sz w:val="44"/></w:rPr><w:t>XX项目投标文件 &amp; 技术方案</w:t></w:r>
  </w:p>
  <w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr>
    <w:r><w:t>第一章 &lt;总体概述&gt;</w:t></w:r>
  </w:p>
  <w:p>
    <w:r><w:rPr><w:i/></w:rPr><w:t>本项目旨在</w:t></w:r>
    <w:r><w:t xml:space="preserve">打造一款“中文 &lt;标书&gt; 场景”的高性能 Word 工具，支持大文件。</w:t></w:r>
  </w:p>
  <w:tbl>
    <w:tblGrid><w:gridCol w:w="4000"/><w:gridCol w:w="3000"/><w:gridCol w:w="2000"/></w:tblGrid>
    <w:tr><w:tc><w:p><w:r><w:t>序号</w:t></w:r></w:p></w:tc>
          <w:tc><w:p><w:r><w:t>设备名称</w:t></w:r></w:p></w:tc>
          <w:tc><w:p><w:r><w:t>数量</w:t></w:r></w:p></w:tc></w:tr>
    <w:tr><w:tc><w:tcPr><w:gridSpan w:val="2"/></w:tcPr><w:p><w:r><w:t>合计（跨两列）</w:t></w:r></w:p></w:tc>
          <w:tc><w:p><w:r><w:rPr><w:b/></w:rPr><w:t>42</w:t></w:r></w:p></w:tc></w:tr>
  </w:tbl>
  <w:p><w:pPr><w:jc w:val="center"/></w:pPr>
    <w:r><w:drawing><wp:extent cx="1905000" cy="1270000"/>
      <a:blip r:embed="rId8"/></w:drawing></w:r>
  </w:p>
  <w:sectPr><w:pgSz w:w="11906" w:h="16838"/>
    <w:pgMar w:top="1440" w:bottom="1440" w:left="1800" w:right="1800" w:header="851" w:footer="992"/>
  </w:sectPr>
</w:body>
</w:document>"""

rels_xml = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId8" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/img1.png"/>
<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/img2.jpg"/>
</Relationships>"""

with zipfile.ZipFile(os.path.join(DATA, "mini_bid.docx"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="xml" ContentType="application/xml"/>
<Default Extension="png" ContentType="image/png"/>
<Default Extension="jpg" ContentType="image/jpeg"/>
</Types>""")
    z.writestr("_rels/.rels", """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>""")
    z.writestr("word/document.xml", document_xml)
    z.writestr("word/_rels/document.xml.rels", rels_xml)
    z.writestr("word/media/img1.png", img1)
    z.writestr("word/media/img2.jpg", img2)
print("mini_bid.docx written")
