# V1.2 下载器测试报告

测试日期：2026-11-15
环境：Windows 11 + MinGW-w64 GCC 16.2.0（WinLibs，静态链接）+ Node 22 + Python 3.13
构建目录：`build-win`，产物 `build-win/bin/modelforge.exe`（约 7.4 MB）
服务：`http://127.0.0.1:8080`，WebSocket `ws://127.0.0.1:8081/ws`

## 1. 编译与单元测试

```text
cmake --build build-win -j 4        -> 成功
build-win/bin/modelforge_tests.exe  -> All core tests passed
```

新增单元测试覆盖：

- Hugging Face 仓库 ID / 页面 URL / `@revision` 解析
- 非法仓库输入拒绝
- `..`、绝对路径、盘符、Windows 保留名拒绝
- 目标路径必须位于下载目录内
- 文件分类（safetensors / 分片索引 / 词表 / GGUF）
- `/resolve/` URL 编码
- Token 日志脱敏（`hf_` 与 `Bearer`）

## 2. 自动化集成测试（真实 HF 仓库）

命令：

```powershell
.venv\Scripts\python.exe tests\integration\download_smoke.py --base-url http://127.0.0.1:8080 --full
```

结果：**14/14 通过**

| # | 用例 | 结果 | 真实证据 |
|---|---|---|---|
| 1 | 解析公开仓库 | 通过 | `hf-internal-testing/tiny-random-LlamaForCausalLM`，10 文件，14,882,707 字节，commit `9fb191250dd5` |
| 2 | 从页面 URL 提取仓库与分支 | 通过 | `/tree/main` 解析为 `@main` |
| 3 | 不存在仓库错误 | 通过 | `HF_TOKEN_REQUIRED`，信息明确列出“不存在/私有/Token 无效”三种可能 |
| 4 | 路径穿越拒绝 | 通过 | `UNSAFE_PATH`，`../../escape.bin` 在任何网络/磁盘操作前被拒 |
| 5 | 单文件下载 + SHA256 | 通过 | 4,131,280 字节，哈希与仓库 LFS 摘要一致 |
| 6 | 多分片下载 + 自动导入 | 通过 | `bumblebee-testing/tiny-random-GPT2Model-sharded`，3 个分片 + 索引 + 配置，自动导入模型 `model_1a1dd97ae26dc32fc` |
| 7 | 重复下载跳过 | 通过 | 6 个文件全部“本地文件已存在且大小一致，跳过下载” |
| 8 | 暂停 / 断点 / 继续 | 通过 | 断点 636,763 字节；暂停后大小稳定；继续完成 4,131,280 字节 |
| 9 | 取消 / 保留分片 / 重试 | 通过 | 取消时 422,734 / 4,131,280；分片保留；重试后完成 |
| 10 | GGUF 仓库识别与校验 | 通过 | `ggml-org/tiny-llamas/stories260K.gguf`，`kind=gguf`，GGUF 结构 `valid=true` |
| 11 | 错误 Token 校验 | 通过 | `whoami-v2` 返回 401 → `TOKEN_INVALID` |
| 12 | 任务列表 / 详情一致 | 通过 | 27 个任务；已完成文件大小与磁盘一致 |
| 13 | 下载完整仓库 | 通过 | 10 文件（含 README、ONNX）14,882,707 字节，大小全部匹配 |
| 14 | 哈希校验失败处理 | 通过 | 故意下发错误 SHA256：重试 5 次后 `failed`，分片与文件均被删除，未标记成功 |

真实产物示例：

```text
models/_download_tests/single/model.safetensors        4,131,280 字节
models/_download_tests/sharded/model-0000{1,2,3}-of-00003.safetensors
models/_download_tests/sharded/model.safetensors.index.json
models/_download_tests/gguf/stories260K.gguf           1,185,376 字节
models/_download_tests/full/                           10 个文件
```

## 3. 进程重启恢复

步骤：

1. 创建 180 KB/s 限速任务，运行到 723,588 字节。
2. 直接 `Stop-Process -Force` 终止服务（模拟异常退出）。
3. 重新启动服务。

结果：

```text
recovered: paused 1,749,537 "应用重启，下载断点已保留，可点击继续恢复"
models/HfRestartFinal/model.safetensors.mfpart  1,806,836 字节
resume -> completed 4,131,280
```

说明：捕获时数据库记录的字节数滞后于磁盘（1.2 秒持久化节流），恢复使用 `.mfpart` 实际大小，最终文件完整。

## 4. CLI 与服务并存

```text
modelforge download pause <id>   -> 已暂停 8%
Web API                          -> paused 449,993 字节
modelforge download resume <id>  -> 下载中 10%
Web API                          -> completed 4,131,280 字节
```

CLI 创建任务、服务持锁执行：

```text
modelforge download hf --repo bumblebee-testing/tiny-random-GPT2Model-sharded \
  --output .../models/HfCliTest --concurrency 2 --weights-only
-> 8 个文件，682.4 KB，完成
```

退出码 0；未出现两个进程同时写同一任务。

## 5. 并发与实时事件

两个任务同时创建（分片仓库 + GGUF），各自并发 2 / 1：

```text
A completed 337,852  B completed 1,185,376
WebSocket 事件：download.created×2, download.log×16, download.progress×5, download.completed×2
```

## 6. WebSocket 断线重连

通过 Chrome DevTools Protocol 模拟断网/恢复：

```text
before:   实时通道已连接
offline:  实时通道离线
restored: 实时通道已连接
```

页面无 Vue 运行时异常，任务状态在断线期间由接口轮询保持一致。

## 7. HTTP 状态分类

独立 HTTP 引擎测试（真实外部端点）：

```text
429 -> status=429 retryable=true  "请求被限流（HTTP 429）"
503 -> status=503 retryable=true  "HTTP 503"
404 -> status=404 retryable=false "文件不存在（HTTP 404）"
```

## 8. Token 存储

```text
PUT /huggingface/token {hf_testtoken...} -> scheme=dpapi, masked=hf_tes…cdef
数据库存储长度 380 字符（Base64 密文），不含明文 "hf_testtoken"
POST /huggingface/token/validate -> valid=false, TOKEN_INVALID（无效 Token 被正确拒绝）
清除后 configured=false
```

## 9. 未覆盖 / 未验证项

| 项 | 状态 | 原因 |
|---|---|---|
| 真实私有仓库下载 | 未验证 | 测试环境没有可用的私有仓库凭据；401/403 映射与 Token 校验已验证 |
| Hugging Face 真实 429 | 未触发 | 正常测试流量未触发限流；429 分类、指数退避与不可重试错误已在引擎层验证 |
| 超大模型（>10 GB） | 未验证 | 避免不必要的网络与磁盘消耗；断点、限流与小文件并发已验证 |
| 不支持 Range 的服务器 | 未实测 | 代码在收到 200 时安全回退为完整下载并拒绝拼接旧分片；HF 全部返回 206 |
| 非 Windows 平台 | 未实测 | 当前 WinHTTP 实现为 Windows 专用；其他平台返回明确“未启用”错误 |

## 10. 变更文件清单

新增：

```text
backend/include/modelforge/http_client.hpp
backend/include/modelforge/hf_client.hpp
backend/include/modelforge/download_manager.hpp
backend/src/http_client.cpp
backend/src/hf_client.cpp
backend/src/download_manager.cpp
frontend/src/views/DownloadsView.vue        （重写）
tests/integration/download_smoke.py
docs/huggingface-downloader.md
docs/test-report-downloader.md
```

修改：

```text
CMakeLists.txt                    新增源文件、winhttp/crypt32 链接、测试链接
backend/include/modelforge/api_server.hpp
backend/src/api_server.cpp        HF 与下载路由、Token 校验
backend/include/modelforge/app.hpp
backend/src/app.cpp               下载管理器生命周期、download CLI
backend/src/main.cpp              download 子命令与帮助
backend/src/database.cpp          V1.2 表结构与迁移版本 2
backend/src/crypto.cpp/.hpp       Base64 解码（DPAPI 存储）
backend/src/hf_client.cpp         文件分类与 URL revision 修复
tests/unit/core_tests.cpp         HF 解析、路径安全、脱敏测试
frontend/src/api/client.ts        下载状态标签与配色
frontend/src/views/GgufEditorView.vue  支持 ?path= 跳转
README.md / docs/api.md / docs/feature-matrix.md / docs/architecture.md / docs/schema.sql
```

## 10.1 补充验证：S2-pro 架构诊断（2026-11-15 13:40）

用户导入 `<workspace>/S2-pro` 后创建转换任务被拒绝。检查结论：

- `config.json` 没有 `architectures` 字段，`model_type=fish_qwen3_omni`，并包含 `audio_decoder_config` 与 `text_config`。
- 权重索引有 358 个张量，前缀为 `text_model.model.layers.*`（融合 `attention.wqkv`）与 `audio_decoder.*`。
- 当前 llama.cpp（commit `31953bcf`）注册了 `FishOmniModel`（Drama 3）、`Qwen3OmniMoeForConditionalGeneration` 等相近但不同的架构，没有 `fish_qwen3_omni`；`convert_hf_to_gguf.py` 还要求 `architectures` 字段。
- 因此转换被正确拦截，不是 ModelForge 的下载/扫描/任务队列故障。

改进后接口返回：

```text
code: ARCH_UNSUPPORTED_CUSTOM
message: 该模型使用自定义/多模态复合架构（model_type=fish_qwen3_omni，包含 音频解码器、分离的文本子配置）……
suggestion: 该架构需要专用转换器与推理引擎……ModelForge 不会生成可能损坏的 GGUF
```

普通 Llama 模型仍能创建转换任务（`task_1a1ddbc717bb7ab1e`，创建后立即取消），诊断改动无回归。

## 11. 已知缺陷与修复记录

| 发现 | 修复 |
|---|---|
| 删除刚完成的任务会导致 `std::thread` 析构于 joinable 状态，进程终止 | `remove()` 先取走 Job，`finished` 时 join，否则 detach，由线程持有的 `shared_ptr` 延长生命周期 |
| 取消任务时进度错误显示 100% | 取消时以数据库 `downloaded_size` 聚合为准 |
| 失败任务进度显示 100% | 失败时只累计 `completed` 文件大小 |
| 非持锁 CLI 读取到旧进度 | 仅在持有下载锁时使用内存进度，否则读取 SQLite |
| 下载进度列 `progress` 恒为 0 | 由真实字节数实时计算 |
| 初始 `/tree/dev` 无法提取 revision | 段数判断从 `>=5` 改为 `>=4` |
| `tokenizer.model` 被误分类为 tokenizer | 词表后缀优先判断 |
