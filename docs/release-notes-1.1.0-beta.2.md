# ModelForge 1.1.0-beta.2

发布日期：2026-11-25
上一个版本：[1.1.0-beta.1](https://github.com/Lynx3211463259/ModelForge/releases/tag/v1.1.0-beta.1)
完整提交范围：`v1.1.0-beta.1..v1.1.0-beta.2`

> **这是预发布版本（Pre-release）。** 建议先阅读 1.1.0-beta.1 的发布说明了解数据集下载、
> 多后端并装、离线资源库、依赖自检与启动更新检查。beta.2 聚焦量化体积估算、弹窗交互修复
> 与平台范围收敛，不包含新的依赖或运行时格式变更。

## 新增功能

- **量化方案体积估算**：模型转换页的量化下拉框基于**当前模型**动态展示每种方案的预计
  输出体积，并标注占“不量化（当前中间精度）”的百分比，例如
  `Q4_K_M · 约 626.9 KB（占不量化 F16 的 30.5%）`。切换模型、中间精度或量化方案时结果
  同步更新；缺少参数规模/权重体积或位宽数据时给出明确提示，不显示固定或误导性数值。
  量化实验室的卡片与图表同步显示占比。

## 问题修复

- 修复点击“浏览”后目录/文件选择器被其宿主弹窗遮挡、必须关闭当前窗口才能操作的问题：
  弹窗现在共享一个层级栈，后打开的永远置顶；Esc 只关闭最上层；滚动锁持续到最后一层关闭；
  关闭后焦点回到触发按钮。
- 修复预发布版本无法找到自身安装包（`ASSET_NOT_FOUND`）的问题：更新检查优先按完整版本
  （`1.1.0-beta.x`）匹配资产，再回退数字版本。

## 平台范围

- 项目不再包含任何 iOS 相关构建脚本、平台判断、前端资源与文档内容；发布资产仍覆盖
  Windows x64、Linux x64/arm64、macOS x64/arm64、Android x64/arm64、openEuler x64/arm64，
  共 9 个 ZIP，并附 `SHA256SUMS.txt`。
- Windows 为本机构建与真实链路验证；POSIX 目标为交叉编译/链接与魔数校验，未实机运行。
- macOS 产物未签名，分发前需自行 codesign + notarize。

## 其他值得关注的变更

- 量化体积为参数规模 × 位宽的估算，实际以转换结果为准；Q8_0 描述不再写死百分比。
- 预发布标签的数字部分必须高于当前版本，正式 1.1.0 发布后预发布用户会收到升级提示。

## Downloads

<!-- DOWNLOADS:START -->
Choose the archive for your operating system and architecture. All builds are ZIP files.

### Windows

- [Windows x64 — portable](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-win64.zip)

### macOS

- [macOS Apple Silicon (arm64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-macos-arm64.zip)
- [macOS Intel (x64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-macos-x64.zip)

### Linux

- [Linux x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-linux-x64.zip)
- [Linux arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-linux-arm64.zip)

### Android

- [Android arm64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-android-arm64.zip)
- [Android x64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-android-x64.zip)

### openEuler

- [openEuler x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-openeuler-x64.zip)
- [openEuler arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.2/ModelForge-1.1.0-beta.2-openeuler-arm64.zip)


Verify your download with `SHA256SUMS.txt` in the Assets section below.
<!-- DOWNLOADS:END -->
