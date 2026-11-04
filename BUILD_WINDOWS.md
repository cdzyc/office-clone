# Windows 本地构建与测试指南

> 适用：office-clone M3（核心引擎已在沙箱全绿；Qt UI 为首次本地构建骨架）
> 前置说明：桌面版当前是**查看器骨架**（打开/浏览/统计），编辑功能在引擎层已验证、UI 接线是 M2.5。

## 一、环境准备（一次）

| 组件 | 版本建议 | 说明 |
|---|---|---|
| Visual Studio 2022 Community | 免费 | 勾选"使用 C++ 的桌面开发"（含 MSVC + CMake） |
| Qt | 6.4 ~ 6.8（在线安装器免费开源版） | 勾选与你编译器匹配的套件：**MSVC 2022 64-bit**（推荐）或 MinGW |
| Python 3 | 任意（可选） | 生成测试夹具用 |

安装 Qt 时记住安装路径，例如 `C:\Qt\6.8.0\msvc2022_64`。

## 二、获取代码

从会话产物区下载 `artifacts/office-clone/` 整个目录，解压到本地（路径不要含中文更稳），例如 `D:\office-clone`。

## 三、构建（三步）

打开 **"x64 Native Tools Command Prompt for VS 2022"**（开始菜单）：

```bat
cd /d D:\office-clone

:: 1) 引擎测试（不需要 Qt，先确认环境 OK）
cmake -B build-core
cmake --build build-core --config Release
build-core\Release\core_tests.exe tests\data
build-core\Release\m2_tests.exe tests\data
build-core\Release\m3_tests.exe tests\data

:: 2) 桌面应用（需要 Qt）
cmake -B build -DOC_BUILD_QT=ON -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64
cmake --build build --config Release

:: 3) 运行（直接拖文件或命令行）
build\Release\office-clone.exe "你的标书.docx"
```

> 若 CMake 找不到 Qt：确认 `CMAKE_PREFIX_PATH` 指向带 `bin\Qt6Core.dll` 的目录。
> 若用 MinGW 套件：在 Qt 自带的 "Qt 6.x.x (MinGW)" 命令行里执行同样命令。

### 首次跑测试夹具（可选，跑引擎测试前）

```bat
python tests\make_fixtures.py
python tests\make_big_docx.py
```

## 四、测试清单（按顺序）

### A. 引擎回归（沙箱已全绿，本地确认编译器差异）

| 命令 | 预期 |
|---|---|
| `core_tests.exe tests\data` | **68 passed, 0 failed** |
| `m2_tests.exe tests\data` | **109 passed, 0 failed** |
| `m3_tests.exe tests\data` | **50 passed, 0 failed** |
| `realdoc_test.exe 大标书.docx` | 打开 ~0.4s；round-trip 结构全对齐 |

### B. 桌面版体验（重点）

用你的 **291.7MB 四川银行标书**测试：

1. **打开**：Ctrl+O → 选中 → 观察状态栏耗时
   - 预期：**< 1 秒出内容**（沙箱 388ms；本地 NVMe 通常更快）
   - 状态栏应显示：`paragraphs=12282 tables=101 images=905 ...`
2. **滚动**：拖动滚动条到文档末尾
   - 预期：全程流畅，无长时间白屏（分页虚拟化只画可见页）
   - 观察：页码指示连续变化，最终约 **590+ 页**
3. **多开**：再开 2~3 个窗口，分别打开标书
   - 预期：互不卡顿；任务管理器看单实例内存（媒体索引模式，应远小于文件体积）
4. **标题/样式**：开头几页应显示标题居中、正文左对齐的基本版式

### C. 已知边界（别当 bug 报）

- 图片显示为 `▣[图片 W×H]` 占位（ImageCache 已就绪，UI 接线在 M2.5）
- 表格暂不渲染（101 张表已解析，渲染在 M2.5/M4）
- 页眉页脚 32 张图未入库（M4）
- 保存菜单未启用（引擎已验证，UI 在 M2.5）

## 五、把结果带回来

记录这四项，直接贴给我：
1. 引擎三套测试的 pass/fail 数
2. 标书打开耗时（状态栏）
3. 滚动到 500 页是否流畅
4. 任务管理器里的内存占用

任何编译错误把**完整报错文本**贴回来，我远程修。

## 六、常见问题

| 现象 | 处理 |
|---|---|
| `Could not find a package configuration file Qt6` | CMAKE_PREFIX_PATH 指到 Qt 套件目录（含 bin/Qt6Core.dll 那层） |
| MSVC 报 C2027/C2065 | 确认在 x64 Native Tools 命令行，不是普通 cmd |
| 打开后窗口空白 | 正常现象需 Ctrl+O 打开文档；启动页有提示文字 |
| 双击 exe 闪退 | 用命令行运行看报错；通常是缺 Qt DLL（用 windeployqt 部署） |
| 中文路径乱码 | 用 `office-clone.exe "完整路径"` 命令行传参 |

> 闪退时先跑：`windeployqt build\Release\office-clone.exe`（Qt 命令行里）
