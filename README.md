# office-clone — C++ 复刻 Word（标书场景优化）

基础版 Word 类 Windows 桌面应用。免费、无冗余功能、专注一件事：
**流畅打开和编辑几百 MB 的标书文档**。

## 获取可运行的 exe（三条路，按省事排序）

- **⭐ 直接下载**：会话产物区 `office-clone-windows-x64.zip`（19MB，解压双击即用，无需安装任何东西）
  —— 由 Linux 沙箱交叉编译（MinGW 13 + Qt 6.8.1 win64）产出
- **免装环境云编译**：上传 GitHub → Actions 自动出包（[CI_PACKAGING.md](CI_PACKAGING.md)）
- **本地编译**：VS2022 + Qt 6（[BUILD_WINDOWS.md](BUILD_WINDOWS.md)）

## 当前进度

- **M1 ✅** 核心引擎（读 .docx / 媒体索引 / 虚拟化 UI 骨架）
- **M2 ✅** 编辑闭环（模型层编辑命令 / .docx 写回 / 字体检测 / 撤销重做）
  - 打开 → 编辑（插入/删除/拆段/加粗/对齐）→ 保存 → 重开，全链路测试通过
  - 自研 DEFLATE 压缩器（python zlib 双向对拍）+ ZipWriter（智能 stored 降级）
  - DocxWriter 产物通过 python zipfile/ElementTree 严格互操作验证
  - FontCheck：仿宋_GB2312 等标书常见缺失字体检测 + 回退建议（不打包字体，ADR A8）
- **M2.6 ✅** 剪贴板（模型层）：跨段复制（保留 run 格式与段落结构）/ 富文本粘贴 /
  跨段删除（body 区段合并 + cell 内）/ 外部纯文本粘贴；全部可撤销
- **M3 ✅** 性能专项：
  - `PngDecode`：零依赖 PNG 解码（8bit Gray/GrayA/RGB/RGBA，全部 5 种 filter，
    CRC 校验，zlib 流）+ 盒式下采样缩略图——python 独立编码器交叉对拍
  - `ImageCache`：按需解码 + LRU 字节预算缓存（ADR A5）；JPEG 经
    `setExternalDecoder` 注入（Windows: WIC/libjpeg-turbo）
  - `Layout`：惰性分页引擎（ADR A6）——`ensurePages(n)` 只排到第 n 页；
    度量注入化（`TextMeasurer`）；脏区失效重排
  - 实测（见 PERF.md）：118MB 标书打开 127ms、首屏 0.01ms、全量分页 532 页 1.6ms、
    **118MB×5 文档同开仅 +14MB RSS**
- **M2.5** UI 编辑接入（光标/选区/键盘 → DocumentEditor；系统剪贴板 → ClipboardFragment；
  ImageCache/Layout 接入 PageView）
- **M3.5** 增量保存 / ZIP64 / 打印 PDF

## 架构

```
app/    Qt 6 UI 层（MainWindow / PageView 虚拟化渲染）   ← 仅此层依赖 Qt
────────────────────────────────────────────────────
core/   文档核心（零外部依赖，纯 C++20）
  Inflate.cpp       DEFLATE 解码器（RFC 1951，手写）
  Deflate.cpp       DEFLATE 压缩器（fixed-Huffman + LZ77）
  ZipReader.cpp     ZIP 只读解析（central directory 索引 + 按条目流式读取）
  ZipWriter.cpp     ZIP 写入（智能压缩：膨胀自动降级 stored）
  XmlPull.cpp       XML 拉式解析器（实体/CDATA/自闭合/注释）
  Document.h/.cpp   文档模型（段落/Run/表格/图片引用/页面设置/字体）
  DocxReader.cpp    .docx → Document（媒体只建索引不解码）
  DocxWriter.cpp    Document → .docx（媒体字节级搬运保真）
  FontCheck.cpp     字体可用性检测 + 标书回退表（ADR A8）
  EditCommands.cpp  模型层编辑命令（扁平段落编址，含表格内段落，全部可撤销）
  Clipboard.cpp     富文本剪贴板片段（跨段复制/粘贴数据模型）
  PngDecode.cpp     零依赖 PNG 解码（全 filter）+ 盒式下采样
  ImageCache.cpp    图片按需解码 + LRU 字节预算缓存
  Layout.cpp        惰性分页引擎（ensurePages/ensureAll/invalidate）
  CommandStack.h    撤销/重做（Command 模式，支持合并）
────────────────────────────────────────────────────
tests/  端到端测试 + 互操作验证 + 大文件性能基准
```

### 性能设计（标书场景）

| 瓶颈 | 对策 | 状态 |
|---|---|---|
| 图片占标书体积 90%+ | 媒体**只建索引**，滚动到可视区才按需解码 + LRU | ✅ 索引式打开已实现 |
| 全量分页排版卡死 | 惰性分页：视口精排 + 后台预排 | 骨架（M2/M3 完整实现） |
| DOM 全量解析 XML | 拉式流式解析 + ZIP 条目懒加载 | ✅ 已实现 |
| 大文件保存慢 | 增量保存（只重写变化条目） | M2/M3 |

### 实测数据（本仓库 bench，g++ 13 -O2）

| 文件 | 构成 | 打开+解析 |
|---|---|---|
| 47 MB | 2030 段 + 30 图（媒体 46.9MB） | **53 ms** |
| 118 MB | 5075 段 + 75 图（媒体 122.9MB） | **129 ms** |

> 图片未解码（架构预期）；媒体按需读取另行计时。

## 构建

### 1) 核心引擎 + 测试（无依赖，任何平台）

```bash
python3 tests/make_fixtures.py        # 生成测试夹具（含迷你标书 docx）
g++ -std=c++20 -O2 -o core_tests tests/core_tests.cpp core/*.cpp
./core_tests tests/data               # 68 项断言

python3 tests/make_big_docx.py        # 生成 47MB 模拟标书
g++ -std=c++20 -O2 -o bench tests/bench.cpp core/*.cpp
./bench tests/data/big_bid.docx
```

CMake（推荐）：

```bash
cmake -B build && cmake --build build
ctest --test-dir build
```

### 2) Windows 桌面应用（Qt 6）

前置：Visual Studio 2022 + Qt 6.4+（Widgets），Qt 在 PATH 中。

```bat
cmake -B build -DOC_BUILD_QT=ON
cmake --build build --config Release
build\Release\office-clone.exe 大标书.docx
```

## 兼容性策略（.docx 读写）

- ✅ 已实现读写 round-trip：正文/粗斜体/下划线/字号/字体(w:rFonts)/对齐/段落样式/基本表格（含跨列）/页眉页脚距/页面设置/内联图片引用/媒体保真
- ✅ 互操作：写出的 .docx 通过 python zipfile（CRC）+ ElementTree（XML 合法性）严格验证
- 🔜 M3：图片解码显示（按需 + LRU）、页眉页脚内容渲染
- ⛔ 降级提示：修订跟踪、宏（.docm）、SmartArt、OLE 嵌入、加密

## 测试与验证

| 套件 | 数量 | 覆盖 |
|---|---|---|
| core_tests（M1） | 68 | Inflate/Zip/XML/DocxReader/CommandStack |
| m2_tests（M2+剪贴板） | 109 | Deflate 压缩/ZipWriter/DocxWriter round-trip/FontCheck/DocumentEditor/剪贴板/跨段删除 |
| m3_tests（M3） | 50 | PNG 全 filter 像素级对拍/下采样盒平均律/ImageCache LRU/Layout 惰性与连续性 |
| verify_interop.py | 26 | python 严格解析器互操作（第三方可读性） |
| perf_suite | — | 自动性能基准（PERF.md 报告：打开/首屏/分页/解码/多文档内存） |

运行：

```bash
python3 tests/make_fixtures.py
g++ -std=c++20 -O2 -o core_tests tests/core_tests.cpp core/*.cpp
./core_tests tests/data
g++ -std=c++20 -O2 -o m2_tests tests/m2_tests.cpp core/*.cpp
./m2_tests tests/data
python3 tests/verify_interop.py
```

## 路线图

- **M1（当前）** 核心引擎 + UI 骨架 + 性能验证 ✅
- **M2** 编辑（输入/格式/撤销）+ .docx 写回 + 图片解码显示
- **M3** 性能专项：惰性分页引擎、按需解码 + LRU、多文档隔离、崩溃恢复
- **M4** 打印/PDF（WYSIWYG）、目录/脚注/分节
- **M5** 多进程隔离、兼容性长尾

## 许可

代码：MIT。Qt 动态链接（LGPL 合规：不静态修改 Qt 本体）。
