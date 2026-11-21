# llama.cpp 官方发行版运行时

ModelForge 自身不编译、不裁剪、不重命名、不重新打包 llama.cpp。程序消费的是
[`ggml-org/llama.cpp`](https://github.com/ggml-org/llama.cpp) 的 **官方 release 资产**：
压缩包按原始字节保留，解压后保留官方文件、目录结构、版本号与二进制内容。

本文说明分发形态、版本识别、更新检测、下载校验与错误处理。

---

## 1. 目录布局

`llamaRoot` 默认是 `<程序目录>/tools/llama.cpp`，可在 **设置** 或 **运行引擎** 页面修改。

```text
tools/llama.cpp/
├── downloads/
│   ├── llama-b12532-bin-win-cpu-x64.zip        # 官方资产，原始字节
│   └── llama.cpp-b12532.zip                    # 官方 tag 源码包（仅缺少转换脚本时）
├── releases/
│   └── llama-b12532-bin-win-cpu-x64/           # 官方 zip 原样解压（52 个文件）
│       ├── llama-cli.exe
│       ├── llama-quantize.exe
│       ├── llama-server.exe
│       ├── llama-bench.exe
│       └── ggml-*.dll / *.dll
├── source/
│   └── llama.cpp-b12532/                       # 官方源码包原样解压
│       ├── convert_hf_to_gguf.py
│       ├── conversion/
│       ├── gguf-py/
│       └── requirements/
└── modelforge-runtime.json                     # ModelForge 侧写（唯一新增文件）
```

`modelforge-runtime.json` 只记录来源，不修改官方文件：

```json
{
  "tag": "b12532",
  "asset": "llama-b12532-bin-win-cpu-x64.zip",
  "backend": "cpu",
  "releaseDir": "...\\releases\\llama-b12532-bin-win-cpu-x64",
  "sourceRoot": "...\\source\\llama.cpp-b12532",
  "sha256": "beb8c051…",
  "size": 25700468,
  "verified": true,
  "installedAt": "2026-11-19T09:41:12Z",
  "source": "github-release"
}
```

## 2. 版本识别

优先级从高到低，任一命中即停止：

| 顺序 | 来源 | 说明 |
| --- | --- | --- |
| 1 | `modelforge-runtime.json` | 由下载/打包流程写入，含 sha256 与来源 |
| 2 | `releases/<资产名>/` 目录名 | 目录名即官方资产名，`/^llama-(tag)-bin-/` 解析版本 |
| 3 | `llama-cli --version` | `version: 7025 (abc1234)` 或 `version: 1.0.0-dev (build 12532, commit …)` |

后端（CPU / CUDA / Vulkan / …）优先取侧写文件，其次按发行目录中的 `ggml-*.dll` 判定，
最后依据设置。未识别的本地开发构建会报 `local_version_unknown`，不会被误判为官方版本。

## 3. 更新检测

`POST /api/v1/llama/update/check` 与 `modelforge llama check`：

1. 拉取 `GET /repos/ggml-org/llama.cpp/releases?per_page=40`（可选 `MODELFORGE_GITHUB_API` 指向镜像）。
2. 更新通道（`llamaChannel`）：
   - `auto`（默认）：最新、且确实发布了可安装资产的构建；
   - `stable`：优先 upstream 标记为正式版的 release，找不到可安装资产时回退到预发布构建并在消息中说明；
   - `nightly`：直接取最新预发布构建。
3. 按当前 `platform`（win）+ `arch`（x64/arm64）过滤资产，再按后端链匹配：
   - `auto`：检测到 NVIDIA → `cuda`；`vulkan-1.dll` 存在 → `vulkan`；始终保留 `cpu` 兜底；
   - 显式指定后端时，链为 `[指定后端, cpu]`。
4. 选择 **第一个包含匹配资产的 release**（因此即使最新稳定 tag 只发布元数据，也不会误报“无资源”）。
5. 比较版本：
   - 同为 `b<数字>` → 数字比较；
   - 同为 `v<semver>` → 逐段比较；
   - 规则不同 → 用 `published_at` 与本地安装时间比较，并在消息中说明。
6. 结果状态：

| status | 含义 | 建议动作 |
| --- | --- | --- |
| `up_to_date` | 已是最新官方版本 | — |
| `update_available` | 有新构建 | 一键更新 |
| `local_newer` | 本地版本高于公开最新版 | 无需更新 |
| `not_installed` | 未找到可用工具 | 安装 |
| `local_version_unknown` | 有二进制但无法识别官方版本 | 重新安装官方包 |
| `asset_mismatch` | 所有 release 都没有匹配当前平台/后端的资产 | 换后端或手动放置 |

检查结果缓存 10 分钟，`{"force": true}` 可强制刷新。

### 3.1 平台资源选择

| 后端 | 官方资产命名 | 说明 |
| --- | --- | --- |
| CPU | `llama-<tag>-bin-win-cpu-x64.zip` | 默认随包分发的形态 |
| CUDA | `llama-<tag>-bin-win-cuda-12.4-x64.zip` | 多个 CUDA 版本时选 `<= 驱动 CUDA 版本` 的最高版本；无法读取驱动则取最低版本（兼容性最好） |
| Vulkan | `llama-<tag>-bin-win-vulkan-x64.zip` | |
| ROCm / SYCL / OpenVINO / OpenCL | 对应 `-win-<flavour>-x64.zip` | 名称随上游变化，按前缀匹配 |
| CUDA 运行时 | `cudart-llama-bin-win-cuda-<ver>-x64.zip` | 与二进制同版本时自动一并下载，解压到同一目录 |

## 4. 下载、校验与安装

1. 下载到 `downloads/<资产名>.part`，支持 HTTP Range 续传与 3 次重试。
2. 校验 `Content-Length` 对应的大小；不一致则删除并报 `SIZE_MISMATCH`。
3. 校验 GitHub API 的 `digest`（`sha256:…`）；不一致删除并报 `CHECKSUM_MISMATCH`。
   官方未提供摘要时退化为“仅大小校验”，并在状态中标注。
4. 原样解压到 `releases/<资产名去扩展名>/`（Windows 使用系统自带 `tar.exe`，回退 PowerShell `Expand-Archive`）。
5. 仅在本地缺少 `convert_hf_to_gguf.py` 时，额外下载官方 tag 源码包到 `source/`。
6. 写回设置：`llamaRoot`、`llamaBackend`、`quantizePath`、`llamaCliPath`、`llamaServerPath`、
   `llamaBenchPath`、`convertScript`。转换、量化、测试与 CLI 调用无需修改。
7. 写入 `modelforge-runtime.json`。

安装任务单实例运行，支持取消（保留已下载分片）。

## 5. 错误码

| code | HTTP | 含义 | 处理建议 |
| --- | --- | --- | --- |
| `NO_NETWORK` | 503 | 无法连接 GitHub / 下载服务器 | 检查代理与防火墙；离线可手动放置官方 zip |
| `API_RATE_LIMITED` | 429 | GitHub API 限流 | 稍后重试 |
| `RELEASE_NOT_FOUND` | 404 | 指定 tag 不存在或无权限 | 核对 tag 与仓库名 |
| `ASSET_NOT_FOUND` | 404 | 该版本没有此资产 | 刷新资源列表 |
| `ASSET_MISMATCH` | 409 | 没有匹配当前平台/后端的资产 | 切换后端或使用其他版本 |
| `SIZE_MISMATCH` | — | 文件大小不符 | 自动删除坏分片后重试 |
| `CHECKSUM_MISMATCH` | — | sha256 不符 | 代理改写了内容或被篡改 |
| `EXTRACT_FAILED` | — | 解压失败 | 确认 `tar.exe` 或 PowerShell 可用 |
| `DIRECTORY_NOT_WRITABLE` | — | 目标目录不可写 | 更换 `llamaRoot` |
| `INSTALL_START_FAILED` | 409 | 已有安装任务 | 等待或取消当前任务 |

## 6. HTTP API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/llama/status` | 本地状态 + 硬件探测 + 当前任务（不联网） |
| GET | `/api/v1/llama/probe` | 后端链、NVIDIA 驱动 / CUDA 版本、Vulkan |
| POST | `/api/v1/llama/update/check` | 检查更新，body `{"force": true}` |
| GET | `/api/v1/llama/releases/{tag}/assets` | 某版本的官方资产与兼容性标记 |
| POST | `/api/v1/llama/install` | 开始下载安装，body `{"tag","asset","includeSource"}` |
| GET | `/api/v1/llama/install/status` | 安装任务进度 + 最新本地状态 |
| POST | `/api/v1/llama/install/cancel` | 取消安装 |
| POST | `/api/v1/llama/refresh` | 重新从磁盘检测工具路径（手动放置官方包后使用） |

## 7. CLI

```powershell
modelforge llama status [--json]
modelforge llama check [--json]        # 0=最新 3=有更新 2=检查失败
modelforge llama assets b12532 [--json]
modelforge llama install [--tag b12532] [--asset llama-b12532-bin-win-cpu-x64.zip]
```

## 8. 离线与镜像

- 离线：把官方 zip 放入 `downloads/` 与解压目录后，用 **重新检测**（`/refresh`）即可；
  安装时也会复用已存在的完整压缩包。
- 镜像：`MODELFORGE_GITHUB_API=https://ghe.example.com/api/v3` 可替换 API 根地址；
  `llamaRepo` 设置可替换仓库（`owner/name`）。

## 9. 打包

```powershell
scripts\package-windows.ps1 -LlamaBackend cpu
scripts\package-windows.ps1 -LlamaTag b12532 -LlamaBackend vulkan
```

脚本行为：

1. 查询官方 release，按 `-LlamaBackend` / `-LlamaArch` 选择资产；
2. 下载官方压缩包到 `downloads/`，按 API 公布的 sha256 校验；
3. 原样解压到 `releases/<资产名>/`，确认 `llama-quantize.exe` 存在；
4. 下载官方 tag 源码包并原样解压到 `source/`（`-SkipLlamaSource` 可跳过）；
5. 写入 `modelforge-runtime.json`。

脚本 **不会** 复制 `llama.cpp/build*` 下的自编译产物。`scripts/build-llama-tools.ps1`
仅用于本地调试，其输出禁止进入发布包。
