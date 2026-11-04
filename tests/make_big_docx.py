#!/usr/bin/env python3
"""生成大体积模拟标书 docx（用于性能冒烟测试）：
  data/big_bid.docx —— N 张随机字节"图片" + M 个段落
用法：python3 make_big_docx.py [图片张数=30] [每张KB=1600] [段落数=2000]
"""
import os
import random
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

n_img = int(sys.argv[1]) if len(sys.argv) > 1 else 30
kb = int(sys.argv[2]) if len(sys.argv) > 2 else 1600
n_para = int(sys.argv[3]) if len(sys.argv) > 3 else 2000

random.seed(42)
rng = random.Random(2026)

words = ["投标", "技术方案", "实施", "质量保证", "工期", "人员", "设备", "报价",
         "售后", "培训", "风险", "合规", "资质", "业绩", "方案", "评审"]

paras = []
for i in range(n_para):
    if i % 500 == 0:
        paras.append(f'<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr>'
                     f'<w:r><w:rPr><w:b/></w:rPr><w:t>第{i//500+1}章 主要投标内容</w:t></w:r></w:p>')
        continue
    body = "".join(rng.choices(words + ["，", "。", "、"], k=rng.randint(40, 120)))
    paras.append(f'<w:p><w:r><w:t xml:space="preserve">{body}</w:t></w:r></w:p>')

# 图片段（引用全部媒体）
img_paras = []
for i in range(n_img):
    img_paras.append(
        f'<w:p><w:pPr><w:jc w:val="center"/></w:pPr>'
        f'<w:r><w:drawing><wp:extent cx="1905000" cy="1270000"/>'
        f'<a:blip r:embed="rIdImg{i+1}"/></w:drawing></w:r></w:p>')

document = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"'
            ' xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"'
            ' xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"'
            ' xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
            '<w:body>' + "".join(paras + img_paras) +
            '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>'
            '<w:pgMar w:top="1440" w:bottom="1440" w:left="1800" w:right="1800"/></w:sectPr>'
            '</w:body></w:document>')

rels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
for i in range(n_img):
    rels.append(f'<Relationship Id="rIdImg{i+1}" '
                f'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" '
                f'Target="media/pic{i+1}.png"/>')
rels.append('</Relationships>')

out = os.path.join(DATA, "big_bid.docx")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml",
               '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
               '<Default Extension="xml" ContentType="application/xml"/>'
               '<Default Extension="png" ContentType="image/png"/></Types>')
    z.writestr("word/document.xml", document)
    z.writestr("word/_rels/document.xml.rels", "".join(rels))
    for i in range(n_img):
        z.writestr(f"word/media/pic{i+1}.png", bytes(rng.randrange(256) for _ in range(kb * 1024)))

print(f"big_bid.docx: {os.path.getsize(out)/1048576:.1f} MB, {n_para} paras, {n_img} images")
