# ModelForge 1.1.0-beta.1

发布日期：2026-11-24
上一个版本：[1.0.1](https://github.com/Lynx3211463259/ModelForge/releases/tag/v1.0.1)
完整提交范围：`v1.0.1..v1.1.0-beta.1`

> **这是预发布版本（Pre-release）。** 功能已通过本机真实链路与单元测试，但数据集镜像、
> 多后端切换与离线包在不同机器/网络下的组合尚未大规模验证。稳定版通道（`stable`）
> 默认不会收到该版本；如需体验，请在设置中切换更新通道，或直接从下方资产下载。

本版本新增四项平台能力（Hugging Face 数据集、llama.cpp 多后端并装、离线资源库、
依赖健康自检），并补齐启动自动更新检查。模型转换与量化链路保持不变。

## 新增功能

- **Hugging Face 数据集下载**：`/api/v1/huggingface/resolve` 与 `/api/v1/downloads`
  支持 `repoType=dataset`，传入 `huggingface.co/datasets/...` 链接会自动识别并切换到
  datasets API。文件按 parquet/arrow/csv/jsonl/json/text/image/audio/video/archive/tensor
  分类，默认保存到新的 `datasetsDir`，完成后登记到离线资源库而不是模型库。并发、
  断点续传、暂停/继续/取消/重试与模型下载一致；下载页提供模型/数据集切换。
- **llama.cpp 多后端并装**：每个官方 release 解压到 `releases/<asset>` 独立目录并登记到
  `backends.json`。运行引擎页新增“已安装后端（并存）”表格，可随时启用或删除；启用只
  改写 `llama-quantize` / `llama-cli` 等工具路径与 `modelforge-runtime.json`，无需重新
  下载。当前后端不可删除，且始终保留至少一个可用后端。后端识别覆盖 `.dll` / `.so` /
  `.dylib` 与官方资产名，Linux/macOS 与静态构建也能正确归类。
- **离线资源库**：统一索引本地模型、数据集与推理后端，实时检测路径存在性与体积；
  可导出 `modelforge-offline-bundle.json` + `SHA256SUMS.txt`，可选复制全部文件，便于
  拷贝到无网络机器；导入支持 `basePath` 重定位与逐文件 SHA256 校验，并给出缺失/校验
  失败清单。移除条目只删索引，不删除磁盘文件。
- **依赖健康自检**：20 项检查覆盖数据库完整性、5 个目录存在与可写、磁盘空间、Python
  与转换依赖、`convert_hf_to_gguf.py`、5 个 llama.cpp 工具、后端清单、离线资源完整性、
  监听配置与前端静态目录；可选 Hugging Face Token 网络探测（默认完全离线）。提供
  `ensure_dirs`、`repair_tools`、`refresh_backends`、`rescan_library` 四个白名单修复。
- **启动自动更新检查**：设置项 `autoCheckUpdates`（默认开启）同时检查 ModelForge 与
  llama.cpp；仅有可更新项时在页面顶部显示横幅（组件、当前 → 最新版本、通道、资产大小）
  与“更新”按钮，点击即触发对应流程并在横幅内显示进度。离线、限流、检测失败或已是最新
  版本时静默处理，不阻断启动，也不显示横幅。结果按浏览器会话缓存 10 分钟。

## 优化改进

- 新增 `datasetsDir` 设置项与图形化目录选择，数据集与模型权重分开存放。
- 设置页“软件更新”区块新增启动自动检查开关，并说明 ModelForge 使用上方通道、
  llama.cpp 通道位于“运行引擎”页。
- 健康检查与资源库操作结果都带有明确的结构化字段，界面不再用模糊文案代替状态。
- `download_tasks` 增加 `repo_type` 列（附加迁移，旧数据库启动时自动补齐）。

## 问题修复

- 修复后端目录识别只认 Windows `.dll`、导致 Linux/macOS 官方资产无法归类的问题。
- 修复模型目录中存在额外文件（`README.md`、`.gitattributes`、分片外文件）时离线库
  被误报“大小不一致”的问题，现在只在实际占用小于记录值时提示。
- 资源库与健康检查的导出目录、资源目录做了自包含校验，禁止把离线包写进资源目录内部。

## 删除或弃用的功能

- 无功能被移除或弃用。
- 行为变更：`/api/v1/huggingface/resolve` 与 `/api/v1/downloads` 保持向后兼容，
  `repoType` 缺省为 `model`；只有显式指定 `dataset` 或使用 `/datasets/` 链接才会走
  数据集分支。

## 其他值得关注的变更

- 发布资产覆盖 Windows x64、Linux x64/arm64、macOS x64/arm64、Android x64/arm64、
  openEuler x64/arm64，共 9 个 ZIP，并附 `SHA256SUMS.txt`。
- Windows 为本机构建与真实链路验证；POSIX 目标为交叉编译/链接校验，未实机运行。
- macOS 产物未签名，分发前需自行 codesign + notarize；iOS 仍受 SDK/签名限制，
  仅保持源码层面的同步与语法校验意图。
- 验证记录见 `docs/test-report-datasets-library-health.md`。
- 预发布版本号形如 `1.1.0-beta.1`，更新检查按数字部分比较；正式 `1.1.0` 发布后，
  预发布用户会收到正式版更新提示。

## Downloads

<!-- DOWNLOADS:START -->
Choose the archive for your operating system and architecture. All builds are ZIP files.

### Windows

- [Windows x64 — portable](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-win64.zip)

### macOS

- [macOS Apple Silicon (arm64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-macos-arm64.zip)
- [macOS Intel (x64)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-macos-x64.zip)

### Linux

- [Linux x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-linux-x64.zip)
- [Linux arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-linux-arm64.zip)

### Android

- [Android arm64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-android-arm64.zip)
- [Android x64 (Termux)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-android-x64.zip)

### openEuler

- [openEuler x64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-openeuler-x64.zip)
- [openEuler arm64 (static musl)](https://github.com/Lynx3211463259/ModelForge/releases/download/v1.1.0-beta.1/ModelForge-1.1.0-beta.1-openeuler-arm64.zip)

iOS: no distributable build (signing required; no long-lived background HTTP service). Source stays syntax-checked only.

Verify your download with `SHA256SUMS.txt` in the Assets section below.
<!-- DOWNLOADS:END -->
