# 只需下载：GitHub Actions 自动出包指南（3 步）

> 你要做的：**建仓库 → 拖文件 → 下载 exe**。编译、测试、打包全部由 GitHub 服务器自动完成（免费）。
> 全程约 15 分钟（其中你的操作 3 分钟，其余是服务器编译时间）。

## 第 1 步：注册/登录 GitHub（1 分钟）

打开 https://github.com → 注册或登录（个人免费账号即可）。

## 第 2 步：建仓库 + 拖文件（2 分钟）

1. 右上角 **+** → **New repository**
   - 名称：`office-clone`（随意）
   - 可见性：**Public**（Actions 完全免费；Private 也有免费额度）
   - **不要**勾选 README/.gitignore/license（保持空仓库）
   - 点 **Create repository**
2. 在新仓库页面点 **uploading an existing file** 链接
3. 打开下载的 `office-clone` 文件夹，**全选里面的所有内容**（包括 `.github` 这个文件夹！流水线就在里面），
   一次性拖进浏览器上传区（用 Chrome 或 Edge，支持拖文件夹保留结构）
4. 页面底部 **Commit changes**

## 第 3 步：等编译 → 下载（服务器干活，约 5~10 分钟）

1. 仓库页顶部点 **Actions** 标签
2. 左侧点 **Build Windows** → 点最新一条运行记录
3. 等绿色 ✅（约 5~10 分钟；首次要下载 Qt，稍慢）
4. 滚动到底部 **Artifacts** 区域 → 点 **office-clone-windows-x64** 下载 zip
5. 解压 → 双击 **office-clone.exe** → Ctrl+O 打开你的标书

## 你会在 Artifacts 里拿到两个包

| 包名 | 内容 |
|---|---|
| `office-clone-windows-x64` | **可运行的桌面版**（exe + Qt 运行库，已 windeployqt） |
| `perf-and-reports` | 性能报告（PERF.md 等） |

## 流水线每次还会自动做的事（白赚的）

- 跑全部引擎测试：**68 + 109 + 50 项**（Windows 环境的免费回归验证）
- 任何一次代码更新后重新推送，自动重新编译出包

## 常见问题

| 问题 | 处理 |
|---|---|
| Actions 页面空着没跑 | 点左侧 Build Windows → 右侧 **Run workflow** 按钮手动触发 |
| 拖拽文件夹没反应 | 用 Chrome/Edge；或改用 git 命令行推送（见下） |
| 构建失败（红 ❌） | 点进失败的 step，把报错截图/文本发回来，我修 |
| 忘拖 `.github` 文件夹 | Actions 里不会有 Build Windows——把这个文件夹补传，再手动 Run workflow |

## 备选：命令行推送（如果你会用 git）

```bat
cd office-clone
git init
git add .
git commit -m "office-clone M3"
git branch -M main
git remote add origin https://github.com/你的用户名/office-clone.git
git push -u origin main
```

推送完直接去 Actions 页等结果。
