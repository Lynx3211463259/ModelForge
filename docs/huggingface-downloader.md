# Hugging Face 下载器（V1.2）

ModelForge V1.2 新增完整 Hugging Face 模型下载链路：仓库解析、文件筛选、多文件并发、HTTP Range 断点续传、下载校验、暂停/继续/取消/重试、重启恢复、下载后自动接入模型库，以及同一套引擎的 CLI。

本功能没有重写既有转换架构。下载器是新增的独立组件，接入现有 `Database`、`EventHub`、`SystemService`、`ApiServer` 与 `App` 生命周期。

## 1. 参考分析：locmodel

开发前检查了本机工作区与用户目录，**没有找到 locmodel 的源码、安装目录或二进制**，因此：

- 没有读取、复制或反编译 locmodel 的任何实现；
- 没有根据猜测描述其内部结构；
- 交互参考只采用公开可见的通用模式：**仓库解析 → 文件列表与筛选 → 选择目标目录 → 任务化下载 → 进度与暂停/继续**；
- 实际实现完全基于 Hugging Face 官方 REST API、`/resolve/` 下载地址与 HTTP Range 语义。

如果后续提供 locmodel 的项目地址或源码，可以再做一次逐项对照，补充差异分析；当前文档不会虚构其内部机制。

## 2. 总体架构

```text
Vue 下载中心 (/downloads)
  │ REST /api/v1/huggingface/*, /api/v1/downloads/*
  ▼
ApiServer
  ├── HfClient           仓库地址解析、仓库元数据、递归文件树、Token 校验
  ├── DownloadManager    任务状态机、并发调度、断点、校验、自动导入、事件
  │     ├── HttpClient   WinHTTP：HTTPS、重定向、Range、流式写盘、限速、取消
  │     ├── Database     hf_repositories / download_tasks / download_files / download_logs / download_settings
  │     ├── EventHub     download.progress / .log / .completed / .failed ...
  │     └── SystemService 下载完成后扫描并导入模型库
  └── CLI                modelforge download ...
```

关键设计：

- **下载引擎独立于 HTTP 控制器**：`DownloadManager` 不依赖 httplib，CLI 与 Web 共用同一实现。
- **单调度器**：`data/downloads.lock` 跨进程互斥，只有持有锁的进程执行下载；另一个进程只读写 SQLite，并通过状态字段传递控制命令。
- **临时文件与最终文件分离**：下载写入 `<file>.mfpart`，大小与 SHA256 通过后才 `rename` 为最终文件。
- **进度只来自真实字节**：任务进度由磁盘上的分片大小与数据库中的 `downloaded_size` 计算，不使用定时器模拟。

## 3. 仓库解析

`HfClient` 支持：

| 输入 | 示例 |
|---|---|
| 仓库 ID | `Qwen/Qwen2.5-0.5B-Instruct` |
| 完整页面地址 | `https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct` |
| 带分支的页面地址 | `https://huggingface.co/owner/model/tree/dev` |
| 显式 revision | `owner/model@v1.0` 或 `revision` 字段 |
| 私有仓库 | 配置 Access Token 后解析 |
| 需要接受协议的仓库 | 返回 `HF_FORBIDDEN`，提示先接受许可 |

使用的官方接口：

```text
GET https://huggingface.co/api/models/{repo}?revision={rev}&blobs=true
GET https://huggingface.co/api/models/{repo}/tree/{rev}?recursive=true&expand=true
GET https://huggingface.co/{repo}/resolve/{rev}/{path}
GET https://huggingface.co/api/whoami-v2           # Token 校验
```

文件大小与 LFS SHA256 来自 `tree` 接口（`lfs.oid`，仅 64 位十六进制才当作 SHA256）。**不会把 ETag 一律当作 MD5 或 SHA256**；只有元数据明确给出 64 位摘要时才校验。

错误映射：

| 情况 | 错误码 |
|---|---|
| 仓库不存在 / 私有 / Token 无效（HF 统一 401） | `HF_TOKEN_REQUIRED` |
| 需要接受使用协议 | `HF_FORBIDDEN` |
| revision 不存在 | `HF_REPOSITORY_NOT_FOUND`（HF 返回 404 时） |
| 限流 | `HF_RATE_LIMITED` |
| 网络 / DNS / TLS | `HF_NETWORK_ERROR` |
| Token 校验失败 | `TOKEN_INVALID` |

> Hugging Face 对“无权限”和“不存在”的仓库统一返回 401，因此 ModelForge 不会把 401 伪装成“仓库不存在”，而是在错误信息中同时列出三种可能。

## 4. 文件列表与筛选

接口返回每个文件的：

- 相对路径、大小、类型（safetensors / pytorch / gguf / config / tokenizer / vocab / index / readme / other）
- LFS SHA256（若有）
- 是否 LFS

Web UI 支持：全选、反选、仅权重、按类型筛选、文件名搜索、`*` 通配包含/排除、按大小显示。默认选中权重、配置、Tokenizer、词表与分片索引，跳过 README 与未知附件。

多分片模型按仓库真实路径逐个下载，索引文件（`model.safetensors.index.json`）作为普通文件处理，不遗漏、不重命名。下载完成后由现有 `ModelScanner` 检查分片完整性。

## 4.1 数据集仓库

`HfClient::parseRepositoryInput` 识别 `datasets` / `spaces` / `models` 路径前缀，
`resolve` 的第五个参数 `repoType` 为空时采用 URL 自动判断结果，否则由调用方显式
指定（`model` / `dataset`）。数据集分支：

| 项目 | 模型仓库 | 数据集仓库 |
| --- | --- | --- |
| 元数据接口 | `/api/models/{repo}` | `/api/datasets/{repo}` |
| 文件树 | `/api/models/{repo}/tree/{rev}` | `/api/datasets/{repo}/tree/{rev}` |
| 下载地址 | `/{repo}/resolve/{rev}/{path}` | `/datasets/{repo}/resolve/{rev}/{path}` |
| 分类 | safetensors/pytorch/gguf/config/tokenizer/vocab/index/readme/other | parquet/arrow/csv/jsonl/json/text/image/audio/video/archive/tensor/readme/config/other |
| 默认目录 | `defaultModelDir` | `datasetsDir` |
| 完成后 | 按 config.json + safetensors 或 GGUF 自动导入模型库 | 写入 `offline_resources`（type=dataset），不进入模型库 |

`download_tasks.repo_type` 决定每个文件使用哪个 resolve 前缀，因此暂停/继续/重试
与重启恢复都沿用同一类型。数据集任务在 `result` 中返回 `kind=dataset`、`path`、
`fileCount`、`totalBytes`、`indexed`。

## 5. 下载引擎

### 5.1 并发

- 每个任务 1–16 个并发文件，默认 4。
- 每个文件只由一个工作线程写入，不存在多线程写同一文件。
- 队列按文件粒度领取（原子索引），单文件失败不会阻塞其他文件。

### 5.2 断点续传

1. 目标文件写入 `<name>.mfpart`。
2. 每次请求带 `Range: bytes=<offset>-`；如果已有 ETag，同时带 `If-Range`。
3. 服务器返回 `206`：追加写入。
4. 服务器返回 `200`（不支持 Range 或 ETag 变化）：截断并从头下载，不会把旧分片拼接成错误文件。
5. 完成后检查大小；有 SHA256 时流式计算并比对。
6. 校验通过后 `rename` 为最终文件；失败则删除分片并按上限重试。
7. 应用异常退出后，`.mfpart` 保留；重启时任务转为 `paused`，点击继续后从文件实际大小恢复。

### 5.3 速度与资源

- 实时速度使用 0.6/0.4 指数平滑，来源为两次采样的真实字节差。
- 支持每任务总限速（令牌桶，所有工作线程共享），单位字节/秒。
- 连接超时 20 秒，读取超时 60 秒。
- 支持取消：`abort` 标志在每次读取块之间检查，暂停/取消会立即中断当前读取。

### 5.4 重试

- 可重试：连接失败、超时、HTTP 408/429/5xx。
- 指数退避：0.5s → 1s → 2s → 4s → 8s，最多 5 次。
- 不可重试：401/403/404，立即失败并给出原因。
- 暂停不是失败：暂停期间的中断不计入重试次数。

### 5.5 完整性

- 完成前校验文件大小。
- 有 64 位 SHA256 时必须匹配。
- 不完整文件、哈希错误的文件永远不会标记 `completed`。
- 校验失败删除损坏分片，重新下载；连续失败进入 `failed`，可手动重试。

## 6. 任务状态与持久化

状态机：

```text
pending → downloading → verifying → completed
     ↘ paused ↗        ↘ failed
       retrying          cancelled
```

控制命令：

| 操作 | 行为 |
|---|---|
| 暂停 | 中断当前读取，保留 `.mfpart`，状态 `paused` |
| 继续 | 从分片实际大小续传 |
| 取消 | 中断工作线程，保留分片，状态 `cancelled` |
| 重试 | 未完成文件重置为 `pending`，已完成文件跳过（含哈希校验） |
| 删除 | 仅删除任务与日志记录，不删除已下载文件；运行中任务必须先取消 |

重启恢复：

- 启动时把 `resolving/downloading/retrying/verifying` 改为 `paused`，错误信息写明“应用重启，下载断点已保留”。
- 设置 `autoResume=true` 时可自动转回 `pending`（默认关闭）。
- 不会把“应用关闭”当作成功或失败。

跨进程：

- `data/downloads.lock` 保证只有一个进程执行下载。
- CLI 与 Web 通过数据库状态同步：CLI `pause` 写 `paused`，持锁进程在 1 秒内检测到并暂停内存中的任务。
- CLI 创建任务后，持锁的 Web 进程会自动接单。

## 7. 数据库迁移

启动时以 `CREATE TABLE IF NOT EXISTS` 幂等创建，写入 `schema_migrations` 版本 2；不修改既有表，不删除数据。

```text
hf_repositories(id, repo_id, revision, commit_hash, repo_type, total_size, file_count,
                license, gated, raw_json, created_at, updated_at)
download_tasks(id, repository_id, repo_id, revision, commit_hash, destination, status,
               total_bytes, downloaded_bytes, progress, speed, concurrency, max_speed,
               retry_count, file_count, completed_files, current_files, selection_json,
               error_message, result_json, created_at, updated_at, completed_at)
download_files(id, task_id, remote_path, local_path, file_size, downloaded_size, etag,
               last_modified, checksum, sha256, status, retry_count, error_message, updated_at,
               UNIQUE(task_id, remote_path))
download_logs(id, task_id, seq, level, message, created_at)
download_settings(key, value, updated_at)
```

完整模式见 [`schema.sql`](schema.sql)。

## 8. REST API

所有接口位于 `/api/v1`，统一 `{ ok, data, error }` 响应。

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/huggingface/resolve` | 解析仓库，返回元数据与文件树 |
| GET | `/huggingface/repository?repo=&revision=` | 重新解析仓库 |
| GET | `/huggingface/files?repo=&revision=` | 仅返回文件列表 |
| GET | `/huggingface/token` | Token 是否配置、掩码、存储方式 |
| PUT | `/huggingface/token` | 保存 / 清除 Token（DPAPI 加密） |
| POST | `/huggingface/token/validate` | 通过 `whoami-v2` 校验 Token |
| GET | `/downloads?limit=` | 任务列表 + 下载设置 + 引擎状态 |
| POST | `/downloads` | 创建下载任务 |
| GET | `/downloads/{id}` | 任务详情（含文件级状态） |
| GET | `/downloads/{id}/files` | 文件级状态 |
| GET | `/downloads/{id}/logs?after=&limit=` | 增量日志 |
| POST | `/downloads/{id}/pause` | 暂停 |
| POST | `/downloads/{id}/resume` | 继续 |
| POST | `/downloads/{id}/cancel` | 取消 |
| POST | `/downloads/{id}/retry` | 重试 |
| DELETE | `/downloads/{id}` | 删除任务记录 |
| GET/PUT | `/downloads/settings` | 并发、限速、校验、自动导入 |

创建任务示例：

```json
{
  "repoId": "bumblebee-testing/tiny-random-GPT2Model-sharded",
  "revision": "main",
  "commitHash": "4fca22a84867...",
  "destination": "C:/models/tiny-sharded",
  "files": [
    {"path": "config.json", "size": 879, "sha256": ""},
    {"path": "model-00001-of-00003.safetensors", "size": 131192, "sha256": "..."}
  ],
  "concurrency": 4,
  "maxSpeed": 0,
  "verifyChecksum": true,
  "autoImport": true,
  "repository": {"repoId": "...", "revision": "main", "commitHash": "...", "license": "mit"}
}
```

实时事件（WebSocket `ws://127.0.0.1:{port+1}/ws` 与 SSE 同时可用）：

```json
{"type":"download.progress","taskId":"dl_...","status":"downloading","progress":42,
 "downloadedBytes":1749537,"totalBytes":4131280,"speed":180000,
 "completedFiles":0,"fileCount":1,"currentFiles":"model.safetensors"}
```

事件类型：`download.created`、`download.progress`、`download.log`、`download.paused`、`download.resumed`、`download.cancelled`、`download.retry`、`download.completed`、`download.failed`。

前端断线后会自动重连 WebSocket，失败时退回 SSE 与 2 秒轮询；任务状态以数据库/接口为准，不依赖事件是否送达。

## 9. CLI

```powershell
# 下载仓库（默认完整仓库）
modelforge download hf --repo Qwen/Qwen2.5-0.5B-Instruct --output .\models --concurrency 4

# 指定分支与限速（字节/秒）
modelforge download hf --repo owner/model --revision dev --max-speed 2097152

# 仅权重（含分片索引）
modelforge download hf --repo owner/model --weights-only

# 指定文件 / 通配
modelforge download hf --repo owner/model --files config.json,model.safetensors
modelforge download hf --repo owner/model --include "*.safetensors" --exclude "*onnx*"

# 私有仓库
modelforge download hf --repo owner/private --token hf_xxx

# 任务管理
modelforge download list
modelforge download pause <task_id>
modelforge download resume <task_id>
modelforge download cancel <task_id>
modelforge download retry <task_id>
modelforge download delete <task_id>
```

CLI 与服务同时运行时，两者不会重复执行同一队列：持锁方负责实际下载，另一方等待并读取状态。

## 10. Token 安全

- Windows 下使用 **DPAPI（`CryptProtectData`，当前用户作用域）** 加密后存入 `download_settings`；数据库中是 Base64 密文，不是明文。
- 非 Windows 平台没有 DPAPI 时会退化为带 `plain:` 前缀的本地存储，并写入警告日志；文档明确说明该降级。
- Token 不写入 URL、不写入前端错误、不写入任务日志；日志输出统一经过 `HttpClient::redact` 二次脱敏。
- 跨主机重定向时移除 `Authorization` 头，避免把 Token 转发给 CDN 之外的第三方。
- 前端只显示掩码（如 `hf_tes…cdef`），不回显完整 Token。

## 11. 下载完成后的模型接入

1. 逐文件确认状态为 `completed`，大小匹配。
2. 目录中存在 `.gguf`：调用 `GgufReader` 校验，写入 `result.kind=gguf` 与校验信息，**不会误当作 Safetensors 模型导入**。
3. 存在 `config.json` + `.safetensors`：调用现有 `SystemService::importModel`，进入模型库，记录来源仓库与 commit。
4. 两者都不满足：`kind=unknown`，说明缺少哪些文件，不伪造成功。
5. 前端详情页可直接跳转到 `/convert?model=<id>` 或 `/gguf?path=<gguf>`。

## 12. 安全边界

| 风险 | 处理 |
|---|---|
| 路径穿越 | 拒绝 `..`、绝对路径、盘符、反斜杠、控制字符、Windows 保留名；最终路径必须位于目标目录内 |
| 文件名注入 | 相对路径逐段校验，不允许空段 |
| 覆盖用户文件 | 只写入用户选择的目标目录；已存在且大小/哈希一致时跳过；不一致时仅替换同名目标文件 |
| 任意文件访问 | 下载 URL 只能由 `repoId + revision + 仓库内相对路径` 生成，API 不接收任意远端 URL |
| Token 泄漏 | DPAPI、掩码、日志脱敏、跨主机重定向去头 |
| 未授权私有仓库 | HF 返回 401/403 时立即失败，不重试 |
| 下载地址伪造 | 仅允许 `huggingface.co` 生成的 `/resolve/` 地址；重定向仅跟随 HTTP 3xx |

## 13. 已知限制

- 仅支持 Hugging Face 模型仓库（`repo_type=model`）；数据集/Spaces 未开放。
- HF 对无权限与不存在均返回 401，无法在客户端 100% 区分。
- 断点续传依赖服务器支持 Range；不支持时按安全策略从头下载。
- 暂停时正在写入的最后一个数据块可能丢失（最多一个读取块），续传会从文件实际大小继续。
- 已配置 Token 的“有效性”需要主动点击验证；公共仓库即使 Token 无效也可能解析成功（HF 行为）。
- 下载完成后不自动启动推理，避免占用资源；可在模型测试页手动执行。
