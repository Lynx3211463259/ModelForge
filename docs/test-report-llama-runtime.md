# llama.cpp 运行时与更新能力：验证记录

验证时间：2026-11-19
环境：Windows 11 · MinGW-w64 GCC 16.2.0 · 官方 release `b12532`

## 1. 编译与单元测试

| 项目 | 结果 |
| --- | --- |
| `cmake --build build-win` | 通过，`build-win/bin/modelforge.exe` |
| `modelforge_tests.exe` | `All core tests passed` |
| `npm run build`（frontend） | 通过，新增 `LlamaRuntimeView-*.js` |

## 2. 版本识别

| 场景 | 命令 | 结果 |
| --- | --- | --- |
| 官方目录 | `llama status` | `b12532` / `CPU` / `directory-scan` |
| 无侧写文件、有 releases 目录 | 临时改名 `releases` | 回退到二进制解析，仍识别为 `b####` |
| 侧写文件 | `install` 后 | `github-release`，含 `sha256=beb8c051…`、`size=25700468` |
| 仅残留旧自编译工具 | 初始状态 | `local_version_unknown`（不会把 `version: 1` 误判为官方版本） |

## 3. 更新检测分支

| 场景 | 触发方式 | 结果 |
| --- | --- | --- |
| 已是最新 | `llama check` | `status=up_to_date`，`已是最新官方版本 b12532。` |
| 无网络 | `MODELFORGE_GITHUB_API=http://127.0.0.1:9` | `NO_NETWORK`：`无法连接 GitHub：发送请求失败: WinHTTP 错误码 12029`，附代理/离线建议 |
| 资源不匹配 | `llama assets v1.0.0` | 该 tag 只有 `nightly-tag.txt`，无 `win-x64` 资产 → 无 `compatible` 项 |
| 稳定通道回退 | `llamaChannel=stable` | `官方稳定版暂无可用资产，已回退到预发布构建 b12532。` |
| 指定资源不存在 | `llama install --asset llama-b12532-bin-win-foo-x64.zip` | `ASSET_NOT_FOUND`：`该版本中没有名为 … 的资源` |
| 缓存结果 | 连续两次 `check` | 第二次命中 10 分钟缓存，`cached=true` |

## 4. 真实下载与校验

- 资产：`llama-b12532-bin-win-cpu-x64.zip`，25,700,468 字节。
- 下载完成后校验 GitHub 公布的摘要 `beb8c051a782fb0970d4ae20fa276a6dfd3e7245685b185244a22570e9e473fa`，一致。
- 解压到 `releases/llama-b12532-bin-win-cpu-x64/`，共 52 个文件，全部为官方文件原名。
- 缓存命中路径：再次安装时复用已校验压缩包（日志 `复用已校验的官方安装包`），不重复下载。

## 5. 设置写回（兼容现有调用流程）

安装完成后 `GET /api/v1/settings`：

```text
llamaRoot      = <程序目录>\tools\llama.cpp
llamaBackend   = cpu
quantizePath   = ...\releases\llama-b12532-bin-win-cpu-x64\llama-quantize.exe
llamaCliPath   = ...\llama-cli.exe
llamaServerPath= ...\llama-server.exe
llamaBenchPath = ...\llama-bench.exe
```

随后 `/api/v1/capabilities` 返回 `conversionReady=true`、38 种量化类型，无 issue。

## 6. 端到端转换（官方二进制）

```text
POST /api/v1/conversions  {modelId: tiny-random-LlamaForCausalLM, quantType: Q4_K_M}
→ SUCCEEDED 100%  output/official-runtime-test.gguf  2,785,056 bytes
```

说明 `convert_hf_to_gguf.py`（官方源码包）+ `llama-quantize.exe`（官方二进制）+ GGUF 校验链路均正常。

## 7. 发布包结构

```text
tools/llama.cpp/
├── downloads/llama-b12532-bin-win-cpu-x64.zip   # 官方原始压缩包
├── releases/llama-b12532-bin-win-cpu-x64/       # 原样解压，52 个文件
├── source/llama.cpp-b12532/                     # 官方 tag 源码包原样解压
└── modelforge-runtime.json                      # tag / sha256 / 来源
```

- 不包含 `llama.cpp/build*` 下的自编译产物。
- `modelforge-runtime.json` 不含绝对路径，解压到任意目录后仍能被目录扫描识别。

## 8. 已知限制

- 打包脚本需要网络下载官方资产（约 66 MB）与源码包（约 41 MB）；离线时可用 `-SkipLlama` 或先放入 `release/.cache/llama/`。
- 官方源码包解压后约 192 MB，包体积因此增大；这是“原样保留目录结构”的代价。
- CUDA 版本的驱动版本判断依赖 `nvidia-smi`；无法读取时选择最低 CUDA 版本以保证兼容。
