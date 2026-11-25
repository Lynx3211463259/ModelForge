# ModelForge 1.0.1

发布日期：2026-11-23
上一个版本：[1.0.0](https://github.com/Lynx3211463259/ModelForge/releases/tag/v1.0.0)
完整提交范围：`v1.0.0..v1.0.1`

本版本以完善自动更新闭环、设置与文件交互体验为主，不引入新的运行时依赖。

## 新增功能

- **软件更新通道**：设置页可选择 `stable` / `nightly` / `auto`。`stable` 只接收正式发布；
  `nightly` 优先预发布并在没有预发布时回退正式版；`auto` 取数字版本最高的构建。
  检查结果新增 `channel` 字段，通道为空时返回 `NO_RELEASE`。
- **删除模型时清理磁盘文件**：确认删除后同时移除模型记录和该模型登记的文件；
  只删除模型目录内且未被其他模型引用的文件，任务/工作流占用时拒绝删除，
  GGUF 产物保留。返回删除数量、释放空间、缺失/共享/失败明细。
- **图形化目录与文件选择器**：设置页路径字段、模型导入、转换输出目录、监控目录
  均可通过内置树形浏览器选择，支持展开/折叠、快捷位置、上一级、绝对路径直达、
  当前选中路径与确认/取消。文件模式带后端校验。
- **临时端口回退横幅**：设置端口被占用时，本次启动临时换用空闲端口，页面顶部显示
  醒目横幅说明原因与实际端口；设置中的端口保持不变，下次启动仍按设置尝试。

## 优化改进

- 版本号统一由仓库根目录 `VERSION` 提供，CMake、POSIX 交叉编译、Windows 打包与发布
  脚本均读取它；单测校验编译版本与文件一致。
- 设置页「当前版本」直接读取运行中程序的版本，不再依赖先检查更新；自更新重启后自动刷新。
- `--port` 与 `--no-browser` 改为仅影响本次启动，不再写回用户设置。
- 设置页显示当前实际端口，并说明端口变更需重启、占用时临时回退不改写设置。
- 设置页新增数据目录/文件浏览按钮，补齐中英文提示文案。
- 本地模型库与导入流程的删除/选择反馈更明确：成功数量、释放空间、警告与失败原因均可见。

## 问题修复

- 修复语言切换后整页被重新挂载、从服务器重载旧设置导致英语未保存、刷新回到中文的问题；
  语言现在切换即生效并立即持久化，双向切换均已浏览器实测。
- 修复非法端口（`80`、`70000`、`0`、非数字）被静默重置为默认值的问题，现在返回
  `400 SETTINGS_INVALID`。
- 修复 Windows 本地化文件错误信息非 UTF-8 导致更新删除流程返回 `INTERNAL_ERROR` 的问题，
  改为稳定的错误码与文本。
- 修复“当前版本”在检查更新前显示 `—` 的问题。
- 删除被占用/只读文件时不再让非法 UTF-8 消息破坏 JSON 响应，且保留记录便于重试。

## 删除或弃用的功能

- 无功能被移除或弃用。
- 行为变更：`DELETE /api/v1/models/{id}` 缺省仍只删除记录（兼容旧客户端），
  只有显式传 `deleteFiles=true` 才同时删除磁盘文件；Web UI 始终使用该模式。

## 其他值得关注的变更

- 发布资产覆盖 Windows x64、Linux x64/arm64、macOS x64/arm64、Android x64/arm64、
  openEuler x64/arm64，共 9 个 ZIP，并附 `SHA256SUMS.txt`。
- Windows 为本机构建与验证；POSIX 目标为交叉编译/链接与静态检查，未实机运行。
- 性能基准：待确认（本版本未运行跨版本性能对比）。
- 行为细节：预发布标签的数字部分必须高于当前版本（如 `v1.0.1-nightly.1`），
  否则版本比较结果相等，仍显示 `UP_TO_DATE`。

## Downloads

<!-- DOWNLOADS:START -->
Choose the archive for your operating system and architecture. All builds are ZIP files.

### Windows

- [Windows x64 — portable](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-win64.zip)

### macOS

- [macOS Apple Silicon (arm64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-macos-arm64.zip)
- [macOS Intel (x64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-macos-x64.zip)

### Linux

- [Linux x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-linux-x64.zip)
- [Linux arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-linux-arm64.zip)

### Android

- [Android arm64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-android-arm64.zip)
- [Android x64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-android-x64.zip)

### openEuler

- [openEuler x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-openeuler-x64.zip)
- [openEuler arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.0.1/ModelForge-1.0.1-openeuler-arm64.zip)


Verify your download with `SHA256SUMS.txt` in the Assets section below.
<!-- DOWNLOADS:END -->
