# ModelForge 架构设计

## 1. 设计原则

1. **真实链路优先**：转换、量化、推理与基准只能来自真实子进程。能力缺失时显式阻断。
2. **C++ 是业务核心**：API、校验、任务状态、持久化、进程生命周期、事件广播全部由 C++ 负责；Python 只运行 llama.cpp 转换脚本。
3. **阶段可审计**：转换与量化分开，日志按任务独立保存，产物生成 manifest 与 SHA256。
4. **不伪造进度**：只有可测量时显示百分比；无法测量时使用 indeterminate 或基于输出文件增长的估算。
5. **资源安全**：大文件流式读取，限制并发，取消整棵进程树，输出先备份。
6. **扩展边界清晰**：转换引擎通过能力检测和命令计划接入，不使用 Shell 字符串。

## 2. 分层

### 2.1 Web UI

位置：`frontend/`

- Vue 3 + TypeScript + Vite。
- Pinia 管理应用状态、能力、设置、实时连接与 Toast。
- Vue Router 管理 13 个功能页面。
- ECharts 用于资源、体积、内存等图表。
- `LogConsole` 仅在客户端做过滤与显示；日志源为后端事件与 JSONL。
- `ProgressBar` 区分 measured / estimated / indeterminate。
- WebSocket 不可用时自动退化到 SSE。

### 2.2 API 层

位置：`backend/src/api_server.cpp`

职责：

- 注册 REST 路由。
- 统一 `{ok,data}` / `{ok,error}` 响应。
- JSON 大小限制。
- 参数转发与错误映射。
- 静态前端托管与 SPA fallback。
- 报告与 GGUF 下载。
- SSE 事件流。

API 不承担领域规则；模型扫描、参数验证、任务调度均位于服务层。

### 2.3 业务服务层

- `ConverterService`：能力检测、架构兼容性、体积估算、推荐、命令预览。
- `TaskManager`：任务创建、队列、状态机、执行、取消、重试、日志、验证。
- `SystemService`：模型库、资源、历史、工作流、量化预设、基准结果。
- `SettingsService`：工具路径、目录、端口、并发与设置导入导出。

### 2.4 执行层

`ProcessRunner`：

- Windows：管道重定向、`CreateProcessW`、`CREATE_SUSPENDED`、Job Object、超时、取消。
- POSIX：`fork/execvp`、独立进程组、管道、SIGTERM → SIGKILL。
- 参数是独立数组，绝不拼接到 Shell。
- stdout/stderr 分线程读取并按行回调。
- tick 回调用于轮询输出文件增长，而不伪造进度。

外部工具：

| 工具 | 用途 | 必需 |
|---|---|---|
| Python + convert_hf_to_gguf.py | 中间 GGUF | 转换必需 |
| llama-quantize | GGUF 量化 | 量化必需 |
| llama-cli | 加载验证、模型测试 | 可选 |
| llama-bench | 性能基准 | 可选 |

### 2.5 存储层

- SQLite（WAL、外键）负责结构化状态。
- JSONL 文件负责完整进程日志，避免每行日志重复入库。
- DB 的 `task_logs` 只保存 WARNING/ERROR 索引。
- 任务报告保存在运行时目录的 `data/reports/{taskId}.json`；运行时目录默认为可执行文件所在目录，而非当前工作目录。
- 大模型文件不进入数据库，仅保存路径、大小、张量信息、哈希等。

## 3. 任务状态机

```text
QUEUED ──► RUNNING(SCANNING) ──► RUNNING(CHECKING)
                                   │
                                   ▼
                            RUNNING(LOADING_CONFIG)
                                   │
                                   ▼
                            RUNNING(CONVERTING)
                                   │
                    ┌──────────────┴──────────────┐
                    │ 无量化                       │ 有量化
                    ▼                             ▼
             RUNNING(VERIFYING)          RUNNING(QUANTIZING)
                    │                             │
                    │                             ▼
                    │                     RUNNING(VERIFYING)
                    └──────────────┬──────────────┘
                                   ▼
                               SUCCEEDED
```

异常终态：`FAILED`、`CANCELED`。排队任务可进入 `PAUSED`。

调度器：

- 读取 `maxConcurrent`。
- 按 `priority DESC, created_at ASC` 取任务。
- 每个任务独立线程调用 `ProcessRunner`。
- 启动前把 DB 状态原子更新为 RUNNING，避免重复调度。
- 进程重启后，将遗留 RUNNING 任务标记为 `PROCESS_RESTARTED`，不谎报完成。

## 4. 进度语义

| 模式 | 含义 | 示例 |
|---|---|---|
| `measured` | 后端可精确测量 | SHA256 百分比、最终 100% |
| `estimated` | 基于输出文件增长估算 | 转换时 `.gguf` 文件大小 / 预计大小 |
| `stage` | 仅阶段进度 | 初始化、加载配置 |
| `indeterminate` | 无法测量，只显示活动状态 | 进程无输出且文件尚未创建 |

UI 必须显示“估算进度”标签，而不是把它包装为精确进度。

## 5. 数据模型

详见 `docs/schema.sql`。

关键关系：

- `models 1 ── n model_files`
- `models 1 ── n conversion_tasks`
- `conversion_tasks 1 ── n model_versions`
- `conversion_tasks 1 ── n task_logs`（警告/错误索引）
- `workflows 1 ── n workflow_steps`
- `models 1 ── n benchmark_results`

## 6. 事件模型

`EventHub` 在进程内广播 JSON：

```json
{
  "type": "task.progress",
  "taskId": "task_...",
  "progress": 42,
  "mode": "estimated",
  "detail": "量化输出中（按输出文件增长估算）",
  "processedBytes": 123456,
  "totalBytes": 456789,
  "elapsedMs": 12000,
  "timestamp": 1794660000000
}
```

传输：

- 主：原生 WebSocket，RFC 6455 握手，独立 `port + 1`，仅回环。
- 备：`GET /api/v1/events` SSE，15 秒心跳。
- 最终兜底：前端轮询任务与日志接口。

## 7. 安全边界

- 输出名清洗：拒绝路径分隔符、Windows 保留名、空名和 `..`。
- 输入/输出路径规范化；输出目录显式创建并检查可写。
- 转换产物下载接口仅按 task id 查找产物。
- HF 下载路径逐段校验：拒绝 `..`、绝对路径、盘符、反斜杠与 Windows 保留名，目标必须位于所选目录内。
- HF Token 使用 Windows DPAPI 加密存储，日志脱敏，跨主机重定向移除 `Authorization`。
- 下载与转换共用“单进程持锁”模型：`downloads.lock` 与 `scheduler.lock` 避免 CLI/Web 重复执行。
- 外部进程参数不经 Shell。
- 覆盖旧文件前创建 `.modelforge-backup-*`，失败时恢复。
- 取消使用 Job Object / 进程组，防止残留转换进程。
- 当前版本固定本机回环地址。
- 远程转换节点与未知二进制插件未启用，避免不完整的安全模型。

## 8. 扩展点

### 8.1 新转换引擎

未来实现统一接口：

```text
detect() -> version / tool paths / supported architectures / quant types
validate(scan, params) -> errors
plan(params) -> stage commands + artifacts
```

C++ 内部 `ConverterService` 已将能力检测和命令构建隔离，便于替换。

### 8.2 远程节点

需要新增：

- 节点身份与双向 TLS。
- 文件访问范围与签名。
- 任务分片与结果回传。
- 断线与幂等恢复。

本版本未提供不安全的“裸远程执行”。

## 9. 已知技术债

- 单一 `ApiServer` 注册文件较大，后续可按领域拆 controller。
- 运行中的外部进程无法真正暂停，只支持取消。
- WebSocket 未加入鉴权与 TLS，因为当前只监听回环地址。
- GGUF 编辑需要完整重写器，当前只读。
- 工作流目前是量化步骤组合，不是任意 DAG。
- 前端未做组件级单元测试；已做构建、路由与真实 API 冒烟验证。
