// oc::PlatformIO — 跨平台文件打开（UTF-8 路径统一入口）
// 问题背景（真机战报 #1）：Windows 的 fopen 按 ANSI 代码页（中文系统=GBK）解读
// const char* 路径；上层（Qt/QString::toStdString）传的是 UTF-8 → 中文文件名乱码 → 打开失败。
// Linux 下 UTF-8 即原生，无此问题。本函数统一兜底：Windows 转 UTF-16 走 _wfopen。
#pragma once
#include <cstdio>
#include <string>

namespace oc {

// mode 仅支持 "rb"/"wb"（当前全部使用场景）；返回 FILE*（失败 nullptr）
FILE* fopenUtf8(const std::string& utf8Path, const char* mode);

} // namespace oc
