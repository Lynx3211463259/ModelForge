# 质量评测 / 资源采样 / 文件夹监控 验证记录

环境：Windows 11，MinGW-w64 GCC 16.2.0，官方 llama.cpp `b12532`（CPU 资产），
模型 `tiny-random-LlamaForCausalLM`（6.18 MB，1 分片）。

## 1. PPL 质量评测

真实调用官方 `llama-perplexity.exe`（未做任何模拟）：

| 场景 | 命令/请求 | 结果 |
| --- | --- | --- |
| 内置样本，ctx 128 | `POST /quality/ppl/run` 单目标 | `completed`，PPL 32274.84，6 chunk，768 token，耗时 0.32 s |
| 内置样本，ctx 256 | 同上，重跑 | PPL 32143.25，3 chunk，768 token，`cpuMs` 687，峰值内存 101.7 MB |
| chunk 解析 | 单行输出 `[1]..,[2]..,[3]..` | 取最大序号（修复前只取第一个，记为 1） |
| 路径匹配 | PPL 以 `/` 传入、版本树以 `\` 存储 | 统一路径键后 `compare` 正确匹配（修复前 `scored=0`） |
| 数据集提示 | `GET /quality/dataset` | 返回 `kind=builtin-sample`、3665 B、`standard=false` 与提示语 |
| 工具缺失 | 清空 `llamaPplPath` | `PPL_NOT_AVAILABLE`，提示先安装官方 llama.cpp |
| 并发 | 任务运行中再次 `run` | `409 PPL_BUSY` |

对比接口返回示例（同一模型的一个产物）：

```json
{"scored": 1, "bestPpl": 32143.2452,
 "rows": [{"quantType": "Q4_K_M", "size": 2789056, "ppl": {"ppl": 32143.2452, "tokens": 768},
           "pplDelta": 0.0, "benchmark": {}}]}
```

说明：随机初始化的小模型 PPL 极高属正常现象；此数据只用于验证链路。

## 2. 任务级资源采样

`tiny-random-LlamaForCausalLM` → `Q4_K_M` 真实转换任务：

```json
{"cpuMs": 8421, "durationMs": 8930, "peakMemory": 677085184,
 "readBytes": 132532229, "writeBytes": 6550784, "cpuPercent": 94.3,
 "stages": [
   {"stage": "CONVERTING", "cpuMs": 8296, "durationMs": 8744, "peakMemory": 677085184, "samples": 43},
   {"stage": "QUANTIZING", "cpuMs": 125, "durationMs": 186, "peakMemory": 19386368, "samples": 1}
 ]}
```

- Windows 下统计改为 Job 对象账户（`TotalUserTime + TotalKernelTime`、`PeakJobMemoryUsed`、
  `Read/WriteTransferCount`）。修复前直接读 `GetProcessTimes`，venv `python.exe` 作为启动器
  只显示 15 ms CPU / 8 MB 内存，明显失真；修复后转换阶段 8.3 s CPU / 677 MB。
- 采样周期约 200 ms（随 `WaitForSingleObject` 超时），每 2 s 落库一次，阶段结束强制落库。
- `GET /conversions/{id}` 内联 `resources`，另有 `GET /conversions/{id}/resources`。

## 3. 文件夹监控

准备 `temp/watch-inbox/TinyWatchModel`（复制现有模型目录，6.2 MB），设置
`watchFolders=[该目录]`、`watchAutoConvert=true`、`watchPresetId=balanced`、`folderWatchEnabled=true`。

| 检查项 | 结果 |
| --- | --- |
| 启用后自动扫描 | 100 ms 轮询内触发，`IMPORTED` + `CONVERT_QUEUED` 两条事件 |
| 自动导入 | 模型库新增 `tiny-random-LlamaForCausalLM`，1 分片 |
| 自动转换 | 按 `balanced` 预设创建任务并成功产出 GGUF |
| 签名去重 | 再次扫描 `imported=0 skipped=1` |
| 强制重扫 | `force=true` 重新导入并登记 |
| 已知目录计数 | 修复前因 `/` 与 `\` 不一致显示 0，修复后 `known=1` |
| 空目录配置 | `WATCH_SCAN_FAILED`（400），提示先配置目录 |
| 目录不存在 | `folders[].exists=false`，事件不产生，状态页显示“不存在” |

## 4. 界面

- `/watch`：深色中文与浅色英文均截图核对，统计卡、设置表单、事件表无错位。
- `/lab` 底部“困惑度质量评测”：上下文/文本输入、运行/取消按钮、进度条与结果表（量化、体积、PPL、Δ、
  Token、耗时、峰值内存、Gen Tok/s）均正常；浅色主题下表格可读。
- `/tasks` 详情新增 CPU 时间、峰值内存、磁盘读写四张统计卡与按阶段汇总行。
- 英文模式 DOM 扫描：`/lab`、`/watch` 中文残留为 0（监控事件 `detail` 为后端诊断文本，按既有边界保留中文）。
