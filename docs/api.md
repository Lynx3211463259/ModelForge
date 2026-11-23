# ModelForge REST API（v1.0.0）

Base URL：`http://127.0.0.1:8080/api/v1`

除文件下载外，请求与响应均为 JSON。所有 `POST` 请求应至少发送 `{}` 或显式 `Content-Length: 0`，以便 HTTP 解析器识别空请求体。

## 1. 响应约定

成功：

```json
{
  "ok": true,
  "data": {}
}
```

失败：

```json
{
  "ok": false,
  "error": {
    "code": "MODEL_INCOMPLETE",
    "message": "缺少 config.json",
    "suggestion": "补齐模型配置后重试"
  }
}
```

常见 HTTP 状态：`200`、`202`、`207`、`400`、`404`、`409`、`500`、`501`。

## 2. 系统

### GET /health

```json
{
  "ok": true,
  "data": {
    "status": "ok",
    "version": "1.0.0",
    "uptimeMs": 12345,
    "websocketPort": 8081,
    "websocketRunning": true
  }
}
```

### GET /system/summary

模型总数、总体积、成功转换次数、队列、资源、最近任务、工程能力。

### GET /system/resources

CPU、内存、磁盘、GPU 文本、OS、主机名、线程数。

### GET /capabilities?refresh=1

动态检测：

- Python 是否可执行
- `torch / numpy / transformers` 是否可导入
- `convert_hf_to_gguf.py` 是否存在
- 支持的架构注册表
- 第一阶段输出类型
- `llama-quantize` 是否可执行及动态量化类型
- `llama-cli` / `llama-bench` 可用性
- 问题列表

### GET /events

Server-Sent Events，`text/event-stream`。每 15 秒心跳。

## 3. 模型

### GET /models?q=&favorite=true&limit=100

返回模型列表。

### POST /models/import

```json
{
  "path": "C:/models/Qwen3-0.6B"
}
```

批量：

```json
{
  "paths": [
    "C:/models/ModelA",
    "D:/models/ModelB"
  ]
}
```

响应：

```json
{
  "ok": true,
  "data": {
    "models": [],
    "errors": [
      { "path": "...", "code": "MODEL_NOT_FOUND", "message": "..." }
    ]
  }
}
```

部分失败返回 `207`。

### GET /models/{id}

包含 `analysis`、`files`、`versions`、`recentTasks`。

### PATCH /models/{id}

可更新 `name`、`favorite`、`notes`、`tags`。

### DELETE /models/{id}

只删除模型库记录，不删除磁盘文件。

### POST /models/{id}/analyze

重新扫描模型目录并更新分析结果。

### POST /models/{id}/validate

重新扫描并返回 `valid` 与完整性检查。

### GET /models/{id}/versions

返回版本树：源模型为根，`nodes` 为扁平列表（含根）。

```json
{
  "ok": true,
  "data": {
    "modelId": "model_...",
    "modelName": "Qwen3-0.6B",
    "root": {"id": "model:model_...", "label": "Qwen3-0.6B", "kind": "source",
             "quantType": "float32", "path": "...", "size": 1234, "children": []},
    "nodes": [
      {"id": "ver_...", "kind": "artifact", "quantType": "Q4_K_M", "taskId": "task_...",
       "path": "...", "size": 2789056, "sha256": "…", "toolVersion": "…",
       "parentId": "", "exists": true, "createdAt": "2026-11-19T…"}
    ],
    "totals": {"count": 13, "size": 36257792, "missing": 0}
  }
}
```

`exists=false` 表示记录指向的文件已不在磁盘上；`missing` 为其数量。

### GET /models/{id}/estimates

```json
{
  "ok": true,
  "data": {
    "parameterCount": 7248,
    "sourceDtype": "float32",
    "options": [
      {
        "type": "Q4_K_M",
        "bitsPerWeight": 4.8,
        "estimatedSize": 27648,
        "estimatedRuntimeMemory": 132000,
        "available": true,
        "description": "4 位 K 混合量化，通用推荐"
      }
    ],
    "method": "按参数规模 × 每权重比特数 + 开销估算……"
  }
}
```

### POST /models/{id}/recommend

```json
{
  "mode": "balanced",
  "availableMemory": 17179869184,
  "availableVram": 8589934592
}
```

`mode`: `quality` | `balanced` | `size`。

## 4. 转换任务

### POST /conversions

```json
{
  "modelId": "model_...",
  "inputPath": "C:/models/Qwen3-0.6B",
  "outputDir": "C:/output",
  "outputName": "Qwen3-0.6B.Q4_K_M",
  "outType": "f16",
  "quantType": "Q4_K_M",
  "threads": 8,
  "overwrite": false,
  "keepIntermediate": false,
  "keepTemp": false,
  "verify": true,
  "deepVerify": false,
  "computeSha256": true,
  "useTempFile": false,
  "noLazy": false,
  "priority": 0
}
```

`outType`: `f32` / `f16` / `bf16`。量化类型必须来自 `/capabilities`。

响应 `202`，返回任务对象。

### GET /conversions?status=&modelId=&limit=100

### GET /conversions/{id}

任务字段：

```json
{
  "id": "task_...",
  "status": "RUNNING",
  "stage": "QUANTIZING",
  "stageLabel": "量化处理",
  "stageDetail": "量化输出中（按输出文件增长估算）",
  "progress": 63,
  "progressMode": "estimated",
  "processedBytes": 1024,
  "totalBytes": 4096,
  "elapsedMs": 12000,
  "errorCode": "",
  "errorMessage": "",
  "outputPath": "",
  "outputSize": 0,
  "sha256": "",
  "verification": {},
  "params": {}
}
```

### POST /conversions/{id}/cancel

请求取消。运行中进程会被终止，排队任务直接变为 CANCELED。

### POST /conversions/{id}/pause

仅 `QUEUED` 支持。运行中外部进程不支持挂起，返回 `409`。

### POST /conversions/{id}/resume

仅 `PAUSED` 支持。

### PATCH /conversions/{id}

```json
{ "priority": 10 }
```

仅排队/暂停任务。

### POST /conversions/{id}/retry

克隆原参数创建新任务；输出覆盖设为 true。

### DELETE /conversions/{id}?logs=true

删除任务记录，可选删除日志。不会删除 GGUF 产物。

### GET /conversions/{id}/logs?afterSeq=&limit=&level=

返回 JSONL 日志条目：

```json
{
  "seq": 12,
  "time": "2026-11-14T23:16:15.600",
  "level": "INFO",
  "message": "..."
}
```

### GET /conversions/{id}/logs/download

下载 JSONL 日志。

### GET /conversions/{id}/result

```json
{
  "ok": true,
  "data": {
    "ready": true,
    "outputPath": "C:/output/model.Q4_K_M.gguf",
    "outputSize": 17184,
    "quantType": "Q4_K_M",
    "sha256": "...",
    "elapsedMs": 9707,
    "verification": {},
    "reportPath": "C:/.../data/reports/task_....json"
  }
}
```

### GET /conversions/{id}/download

下载 GGUF。仅成功任务；文件不存在返回 404。

### GET /conversions/{id}/report

下载转换 manifest JSON。

### GET /queue

```json
{
  "running": 1,
  "queued": 2,
  "paused": 0,
  "succeeded": 3,
  "failed": 1,
  "canceled": 0,
  "maxConcurrent": 1
}
```

### PUT /queue/concurrency

```json
{ "maxConcurrent": 1 }
```

范围 1–8。

## 5. 量化实验室

### GET /quantization/profiles

返回内置与用户预置。

### POST /quantization/profiles

```json
{
  "name": "我的平衡方案",
  "description": "Q5_K_M / 8 threads",
  "params": {
    "outType": "f16",
    "quantType": "Q5_K_M",
    "threads": 8,
    "verify": true
  }
}
```

### PUT /quantization/profiles/{id}

内置预置不可修改。

### DELETE /quantization/profiles/{id}

内置预置不可删除。

### POST /quantization/recommend

```json
{
  "modelId": "model_...",
  "mode": "balanced",
  "availableMemory": 0,
  "availableVram": 0
}
```

### GET /benchmarks

### POST /benchmarks/run

```json
{
  "path": "C:/output/model.Q4_K_M.gguf",
  "modelId": "model_..."
}
```

调用 `llama-bench -m ... -p 512 -n 128 -r 1 -o json`。必须安装 llama-bench，否则 409。

## 6. 模型测试

### POST /tester/run

```json
{
  "path": "C:/output/model.Q4_K_M.gguf",
  "prompt": "Hello",
  "temperature": 0.7,
  "topP": 0.95,
  "topK": 40,
  "maxTokens": 128,
  "context": 2048,
  "gpuLayers": 0,
  "threads": 8
}
```

真实调用 `llama-cli`，超时 5 分钟。返回原始输出与退出码。

## 7. GGUF

### POST /gguf/inspect

```json
{
  "path": "C:/output/model.gguf"
}
```

或：

```json
{
  "taskId": "task_..."
}
```

返回 magic/version/tensorCount/metadataCount/architecture/quantType/metadata/tensors/errors。

### PATCH /gguf/metadata

当前返回 `501 GGUF_EDIT_READ_ONLY`。理由：安全编辑需要重写头部与数据偏移，本版本不做有风险的写入。

## 8. 工作流

```text
GET    /workflows
POST   /workflows
GET    /workflows/{id}      （通过列表实现）
PUT    /workflows/{id}
DELETE /workflows/{id}
POST   /workflows/{id}/run
```

运行：

```json
{ "modelId": "model_..." }
```

每个量化类型创建一个独立转换任务。

## 9. 设置

```text
GET  /settings
PUT  /settings
GET  /settings/export
POST /settings/import
POST /settings/test
```

`PUT /settings` 为局部合并。端口变更返回 `restartRequired=true`。

### POST /settings/test

真实执行：

- `python --version`
- Python 依赖导入
- 转换脚本存在性
- `llama-quantize --help` 能力解析
- `llama-cli` 存在性
- 输出/临时目录写权限

## 10. Hugging Face 下载

所有接口位于 `/api/v1`。详细语义与状态机见 [`huggingface-downloader.md`](huggingface-downloader.md)。

### 10.1 仓库解析

```text
POST /huggingface/resolve
{
  "input": "https://huggingface.co/owner/model/tree/main",
  "revision": "",
  "token": ""            // 可选，留空使用已保存 Token
}

GET  /huggingface/repository?repo=owner/model&revision=main
GET  /huggingface/files?repo=owner/model&revision=main
```

返回：`repoId`、`author`、`name`、`revision`、`commitHash`、`license`、`gated`、`private`、`fileCount`、`totalSize`、`files[]`（`path`、`size`、`category`、`sha256`、`lfs`）。

错误：`INVALID_REPOSITORY`、`HF_TOKEN_REQUIRED`、`HF_FORBIDDEN`、`HF_REPOSITORY_NOT_FOUND`、`HF_RATE_LIMITED`、`HF_NETWORK_ERROR`、`HF_EMPTY_REPOSITORY`。

### 10.2 Token

```text
GET  /huggingface/token                 -> { configured, masked, scheme }
PUT  /huggingface/token { "token": "" } -> 保存或清除；Windows 下 DPAPI 加密
POST /huggingface/token/validate        -> 通过 whoami-v2 校验，返回 valid/username/code
```

Token 不会出现在任何响应正文、日志或 URL 中，只返回掩码。

### 10.3 下载任务

```text
POST   /downloads
{
  "repoId": "owner/model",
  "revision": "main",
  "commitHash": "9fb1912...",
  "destination": "C:/models/model",
  "files": [ { "path": "model.safetensors", "size": 4131280, "sha256": "..." } ],
  "concurrency": 4,
  "maxSpeed": 0,
  "verifyChecksum": true,
  "autoImport": true
}

GET    /downloads?limit=50
GET    /downloads/{id}                 -> 含 files[] 文件级状态
GET    /downloads/{id}/files
GET    /downloads/{id}/logs?after=0&limit=500
POST   /downloads/{id}/pause
POST   /downloads/{id}/resume
POST   /downloads/{id}/cancel
POST   /downloads/{id}/retry
DELETE /downloads/{id}                 -> 仅删除任务与日志，不删除已下载文件
GET    /downloads/settings
PUT    /downloads/settings
```

任务状态：`pending`、`resolving`、`downloading`、`paused`、`retrying`、`verifying`、`completed`、`failed`、`cancelled`。

`result` 字段：

```json
{"kind": "safetensors", "modelId": "model_...", "name": "...", "architecture": "LlamaForCausalLM",
 "parameterCount": 1032272, "summary": "已导入模型库：..."}
```

GGUF 仓库返回 `kind=gguf`、`ggufPath` 与 `verification`；无法识别时返回 `kind=unknown` 并列出缺失文件，不伪造成功。

错误：

| 错误码 | 含义 |
|---|---|
| `INVALID_DESTINATION` | 目标目录缺失或不可写 |
| `DESTINATION_NOT_WRITABLE` | 目录不可写 |
| `NO_FILES_SELECTED` | 未选择文件 |
| `UNSAFE_PATH` | 仓库路径包含穿越、盘符、保留名等 |
| `DOWNLOAD_NOT_FOUND` | 任务不存在 |
| `DOWNLOAD_PAUSE_FAILED` / `RESUME_FAILED` / `CANCEL_FAILED` / `RETRY_FAILED` / `DELETE_FAILED` | 当前状态不允许该操作 |

### 10.4 远程节点

```text
GET /nodes -> 本地节点信息，remoteEnabled=false
```

远程转换节点仍未开放。

## 11. llama.cpp 运行时（官方发行版）

ModelForge 只消费官方 release 资产，不编译、不裁剪、不重命名、不重新打包。详见
[`llama-runtime.md`](llama-runtime.md)。

### GET /llama/status

仅读取本地磁盘，不联网。

```json
{
  "ok": true,
  "data": {
    "runtime": {
      "root": "...\\tools\\llama.cpp",
      "tag": "b12532",
      "versionLabel": "b12532",
      "backend": "cpu",
      "backendLabel": "CPU",
      "installed": true,
      "managed": true,
      "detectionSource": "directory-scan",
      "releaseDir": "...\\releases\\llama-b12532-bin-win-cpu-x64",
      "convertScript": "...\\source\\llama.cpp-b12532\\convert_hf_to_gguf.py",
      "convertScriptAvailable": true,
      "integrity": {"sha256": "…", "size": 25700468, "verified": true, "installedAt": "…"},
      "tools": {"quantize": "…", "cli": "…", "server": "…", "bench": "…",
                "quantizeAvailable": true, "cliAvailable": true,
                "serverAvailable": true, "benchAvailable": true},
      "issues": []
    },
    "probe": {"platform": "win", "arch": "x64", "nvidia": true,
              "nvidiaDriver": "580.88", "cudaVersion": "12.8",
              "vulkan": true, "chain": ["cuda", "vulkan", "cpu"]},
    "job": {"status": "idle", "progress": 0}
  }
}
```

### POST /llama/update/check

请求 `{"force": true}`。返回 `status`、`message`、`suggestion`、`local`、`release`、`asset`、
`remoteTag`、`stableTag`、`fallbackReason`、`backendUpgrade`、`chain`、`available`。

`status` 取值：`up_to_date`、`update_available`、`local_newer`、`not_installed`、
`local_version_unknown`、`asset_mismatch`。

检查本身失败时返回 `NO_NETWORK`(503)、`API_RATE_LIMITED`(429)、`RELEASE_NOT_FOUND`(404)、
`ASSET_MISMATCH`(409) 等错误码。

### GET /llama/releases/{tag}/assets

列出该版本的官方资产，`compatible` 标记是否匹配当前平台/架构/后端链。

### POST /llama/install

```json
{"tag": "b12532", "asset": "llama-b12532-bin-win-cpu-x64.zip", "includeSource": false}
```

`tag` / `asset` 省略时自动使用检查结果。返回 `202` 与任务快照；同一时间只允许一个安装任务。

### GET /llama/install/status

```json
{"ok": true, "data": {"job": {"status": "downloading", "progress": 42,
  "downloadedLabel": "10.3 MB", "totalLabel": "24.51 MB", "speedLabel": "3.1 MB/s",
  "message": "开始下载 llama-b12532-bin-win-cpu-x64.zip", "artifacts": []},
  "runtime": {"…": "…"}}}
```

`job.status`：`idle` | `downloading` | `verifying` | `extracting` | `completed` | `failed` | `cancelled`。

### POST /llama/install/cancel

取消当前安装，已下载分片保留以便续传。

### POST /llama/refresh

重新从磁盘检测工具路径（手动放置官方压缩包后使用）。

## 12. OpenAPI

`GET /openapi.json` 返回 OpenAPI 3.0.3 文档，由运行时实际注册的路由生成，不会与实现漂移。

- `paths`：每条路由（方法 + 规范化路径模板，如 `/models/{modelId}/versions`）。
- `parameters`：路径参数由路由模板推导，查询参数来自路由元数据并带类型。
- `requestBody` / `responses`：引用 `components.schemas`，并统一附带 `400/404/409` 错误响应（`ApiError`）。
- `x-route-coverage`：`{registered, documented, undocumented, complete}`；未登记元数据的路由仍会出现，但标记 `x-documented: false`。
- 当前构建：84 个操作、70 条路径、81 个 schema、`complete: true`。

新增后端路由时，只需用 `routes_->Get/Post/…` 注册（而非 `server_->…`），再在 `backend/src/openapi.cpp` 的元数据表中补一行摘要；缺失时会自动降级并在覆盖率中暴露。

## 13. 质量评测（PPL）

基于官方 `llama-perplexity` 的真实进程评测，同一时间只允许一个评测任务。

### GET /quality/dataset

返回将要使用的评测文本（未指定时返回内置样本路径与提示）。

```json
{"ok": true, "data": {"dataset": {"path": "...\\data\\quality\\sample.txt", "kind": "builtin-sample",
  "bytes": 3665, "standard": false, "exists": true, "hint": "..."},
  "pplPath": "...\\llama-perplexity.exe", "pplAvailable": true}}
```

### POST /quality/ppl/run

```json
{"modelId": "model_...", "targets": [{"path": "...gguf", "versionId": "ver_...", "quantType": "Q4_K_M"}],
 "context": 512, "threads": 8, "datasetPath": ""}
```

`targets` 省略时使用该 modelId 的全部 `model_versions`；`context` 取值 128–4096。返回 `202` 与任务快照；
已有任务运行时返回 `409 PPL_BUSY`，工具缺失返回 `400 PPL_NOT_AVAILABLE`。

### GET /quality/ppl/status

```json
{"status": "running", "progress": 50, "total": 2, "completed": 1, "message": "正在评测 ...",
 "results": [{"quantType": "Q4_K_M", "ppl": 32143.25, "pplStderr": 0, "chunks": 3, "tokens": 768,
              "durationMs": 312, "cpuMs": 687, "peakMemory": 106659840}],
 "logs": [{"level": "INFO", "message": "..."}], "dataset": {"...": "..."}}
```

`status`：`idle` | `running` | `completed` | `failed` | `cancelled`。

### POST /quality/ppl/cancel · DELETE /quality/ppl/{id}

取消当前评测 / 删除一条结果记录。

### GET /quality/ppl?limit=100

历史结果列表（含 `datasetKind`、`context`、`tokens`、`cpuMs`、`peakMemory`）。

### GET /quality/compare?modelId=...

按模型汇总每个产物的最新 PPL、体积、llama-bench 吞吐与相对最优 PPL 的劣化百分比。

```json
{"modelId": "model_...", "modelName": "...", "bestPpl": 32143.2452, "scored": 1,
 "rows": [{"versionId": "ver_...", "quantType": "Q4_K_M", "size": 2789056, "exists": true,
           "ppl": {"ppl": 32143.2452, "tokens": 768, "durationMs": 312, "peakMemory": 106659840},
           "benchmark": {"genTps": 81.2}, "pplDelta": 0.0}],
 "dataset": {"kind": "builtin-sample", "path": "...", "context": 256}, "caveat": "..."}
```

### GET /conversions/{taskId}/resources

任务级资源采样，按阶段返回子进程计数器。

```json
{"stages": [{"stage": "CONVERTING", "stageLabel": "权重转换与 GGUF 写入", "cpuMs": 8296,
             "durationMs": 8744, "peakMemory": 677085184, "readBytes": 132532229,
             "writeBytes": 6550784, "samples": 43, "updatedAt": "..."}],
 "cpuMs": 8421, "durationMs": 8930, "peakMemory": 677085184,
 "readBytes": 132532229, "writeBytes": 6550784, "cpuPercent": 94.3, "sampled": true}
```

Windows 下统计来自 Job 对象账户信息（`TotalUserTime + TotalKernelTime`、`PeakJobMemoryUsed`、
`Read/WriteTransferCount`），因此包含 venv `python.exe` 拉起的真实解释器进程。

## 14. 文件夹监控

### GET /watch/status

```json
{"enabled": true, "intervalSec": 30, "autoConvert": true, "presetId": "balanced",
 "folders": [{"path": "D:/models/incoming", "exists": true, "known": 3, "lastEventAt": "..."}],
 "lastScanAt": "...", "lastScanDurationMs": 41, "nextScanAt": "...", "lastError": "",
 "stats": {"IMPORTED": 12, "CONVERT_QUEUED": 9}, "events": [{"...": "..."}]}
```

### GET /watch/events?limit=100

事件日志：`IMPORTED`、`FAILED`、`CONVERT_QUEUED`、`CONVERT_FAILED`、`CONVERT_SKIPPED`。

### POST /watch/scan

```json
{"force": false}
```

立即扫描；`force: true` 忽略签名，对已处理目录重新导入并重新登记。返回
`{scannedAt, imported, skipped, queued, failed, folders[], errors[], durationMs}`。

## 15. WebSocket

URL：`ws://127.0.0.1:8081/ws`（HTTP 端口 + 1）

服务端仅广播，不要求客户端消息。文本帧为 JSON 事件。

事件示例：

```json
{
  "type": "task.log",
  "taskId": "task_...",
  "log": {
    "seq": 42,
    "time": "2026-11-14T23:16:15.600",
    "level": "INFO",
    "message": "model size = ..."
  },
  "timestamp": 1794660000000
}
```

主要类型：

- `task.created`
- `task.status`
- `task.progress`
- `task.log`
- `task.error`
- `task.completed`
- `task.failed`
- `task.canceled`
- `task.canceling`

## 16. 错误码

| 错误码 | 含义 |
|---|---|
| `MODEL_NOT_FOUND` | 路径不存在 |
| `MODEL_INCOMPLETE` | 配置/权重/Tokenizer/分片不完整 |
| `CONFIG_MISSING` | 缺少 config.json |
| `ARCH_UNSUPPORTED` | 当前转换器未注册该架构 |
| `CONVERTER_MISSING` | 转换脚本缺失 |
| `QUANTIZER_MISSING` | llama-quantize 缺失 |
| `PYTHON_ENV_INVALID` | Python 依赖异常 |
| `OUTTYPE_UNSUPPORTED` | 第一阶段精度不合法 |
| `QUANT_TYPE_UNSUPPORTED` | 量化类型未在当前工具中检测到 |
| `OUTPUT_EXISTS` | 输出存在且未启用覆盖 |
| `OUTPUT_DIR_UNWRITABLE` | 输出目录不可写 |
| `DISK_FULL` | 预计空间不足或磁盘写入失败 |
| `OUT_OF_MEMORY` | 内存不足 |
| `PROCESS_START_FAILED` | 进程无法启动 |
| `PROCESS_EXITED` | 外部进程非零退出 |
| `QUANTIZATION_FAILED` | 量化阶段失败 |
| `GGUF_VERIFY_FAILED` | GGUF 结构验证失败 |
| `DEEP_VERIFY_FAILED` | llama-cli 加载验证失败 |
| `CANCELLED` | 用户取消 |
| `PROCESS_RESTARTED` | 应用重启导致任务中断 |
| `GGUF_EDIT_READ_ONLY` | 元数据写入未开放 |
| `HF_TOKEN_REQUIRED` | 仓库不存在、私有或 Token 无效（HF 统一 401） |
| `HF_FORBIDDEN` | 未接受模型许可协议或权限不足 |
| `HF_REPOSITORY_NOT_FOUND` | 仓库或 revision 不存在 |
| `HF_RATE_LIMITED` | Hugging Face 限流 |
| `HF_NETWORK_ERROR` | 网络、DNS、TLS 失败 |
| `TOKEN_INVALID` | Token 校验失败 |
| `UNSAFE_PATH` | 仓库文件路径被安全策略拒绝 |
| `NO_NETWORK` | 无法连接 GitHub / 下载服务器 |
| `API_RATE_LIMITED` | GitHub API 限流 |
| `RELEASE_NOT_FOUND` | 官方 release tag 不存在 |
| `ASSET_NOT_FOUND` | 该版本没有指定资产 |
| `ASSET_MISMATCH` | 没有匹配当前平台/后端的官方资产 |
| `SIZE_MISMATCH` | 下载文件大小与官方清单不符 |
| `CHECKSUM_MISMATCH` | sha256 校验失败 |
| `EXTRACT_FAILED` | 官方压缩包解压失败 |
| `DIRECTORY_NOT_WRITABLE` | llamaRoot 不可写 |
| `INSTALL_START_FAILED` | 已有安装任务正在运行 |
| `PPL_BUSY` | 已有质量评测任务正在运行 |
| `PPL_NOT_AVAILABLE` | llama-perplexity 不可用或没有目标 |
| `PPL_UNPARSED` | 未能从 llama-perplexity 输出解析困惑度 |
| `WATCH_SCAN_FAILED` | 监控扫描无法开始（未配置目录） |


## 17. 软件更新

所有端点前缀为 `/api/v1/updates`。

### GET /updates/check

按设置项 `updateChannel` 选择发布通道：
`stable`（默认，调用 `/releases/latest`，只取正式版）、`nightly`（读取
`/releases`，优先 `prerelease` 版本，没有预发布时回退到正式版）、
`auto`（读取 `/releases`，取版本号最高的构建，含预发布）。通道会写入
`updateChannel` 设置并通过 `PUT /settings` 持久化。

返回 `currentVersion`、`latestVersion`、`updateAvailable`、`releaseName`、
`releaseNotes`、`htmlUrl`、`publishedAt`、`channel`、`prerelease`、
`assetName`、`assetSize`、`code`、`message`、`suggestion`。
`code`：`UPDATE_AVAILABLE`（发现新版本）、`UP_TO_DATE`（无更新）、
`NO_RELEASE`（所选通道暂无可用版本）、`NETWORK_ERROR`（网络/限流/404）、
`VERSION_PARSE_FAILED`（版本号或响应体无法解析）、
`ASSET_NOT_FOUND`（当前平台无匹配资产）。这些状态以 HTTP 200 返回，由
`code` 区分，前端据此给出对应提示。

### POST /updates/download

请求体可为 `{}`。须先调用 check 且 `updateAvailable` 为 true；否则返回 409
`NO_UPDATE_READY`。成功返回 `jobId`。下载任务在后台执行，状态依次为
`downloading` → `verifying` → `extracting` → `ready`，失败为 `failed`：
`DOWNLOAD_FAILED`、`CHECKSUM_MISSING`、`CHECKSUM_MISMATCH`、`EXTRACT_FAILED`。

### GET /updates/job/{id}

返回 `id`、`status`、`statusLabel`、`progress`、`message`、`code`、
`version`、`assetName`、`downloadedBytes`、`totalBytes`、`scriptPath`。
任务不存在返回 404 `JOB_NOT_FOUND`。

### POST /updates/apply

请求体 `{"jobId": "..."}`。仅 `ready` 状态可应用，否则 409
`UPDATE_NOT_READY`。成功后返回 `message` 与 `scriptPath`，状态变为
`applied`。安装脚本在进程退出后替换文件并重启：Windows 使用
`apply-update.bat`，POSIX 使用 `apply-update.sh`。启动失败返回 500
`APPLY_FAILED`。
