# 测试报告：数据集下载 / 多后端 / 离线资源库 / 依赖自检

日期：2026-11-24 · 版本：1.0.1 + 未发布改动 · 平台：Windows 11（MinGW GCC 16.2.0）

## 1. 单元测试（`ctest` / `modelforge_tests`）

新增用例，全部通过（合计 4 组，110+ 断言）：

| 用例 | 覆盖点 |
| --- | --- |
| `testHfDatasets` | `datasets` URL 解析与类型自动识别；`/api/datasets`、`/datasets/<repo>/resolve` URL；parquet/csv/readme 分类；模型分类不受影响 |
| `testLlamaBackends` | 两个伪 release 目录被扫描；`.dll`/资产名识别 CPU 与 CUDA；切换后端改写 `llama-cli` 路径；活动后端拒绝删除；删除非活动后端并清理目录；注册表跨实例持久化 |
| `testOfflineLibrary` | 数据集目录扫描入索引；磁盘文件数与体积；导出 manifest + SHA256SUMS + 文件副本；导出到资源目录内部被拒绝（`UNSAFE_DESTINATION`）；移除条目保留文件；导入重新登记；`verifyAll` |
| `testHealthCheck` | 报告结构、数据库与目录检查存在、`ensure_dirs` 修复创建目录并返回新报告、未知动作被拒绝、`summary` 返回缓存状态 |

## 2. 真实链路（Windows，实际进程）

| 步骤 | 结果 |
| --- | --- |
| `GET /api/v1/health/check` | 200，19 项，18 ok / 1 warn（离线资源库未索引），耗时 ~6 s（含 Python 依赖探测） |
| `POST /api/v1/library/rescan` | 7 个本地模型 + 1 个 llama.cpp 后端入索引，`missing=0` |
| `POST /api/v1/library/export`（`copy=false`, backend） | 生成 `modelforge-offline-bundle.json`，`itemCount=1` |
| `POST /api/v1/library/import` | `importedCount=1`，`missing=0` |
| `POST /api/v1/health/fix {"action":"rescan_library"}` | 成功，返回修复后报告 |
| `POST /api/v1/huggingface/resolve {"input":"tatsu-lab/alpaca","repoType":"dataset"}` | `repoType=dataset`，3 个文件，24,256,428 字节，parquet 分类正确 |
| `POST /api/v1/downloads`（dataset，2 文件） | 任务 `dl_1a20b63f844b88df3` 完成，`result.kind=dataset`，摘要“数据集已加入离线资源库（2 个文件，23.13 MB）” |
| `GET /api/v1/library?type=dataset` | 1 项，`onDiskBytes=24,254,110`，`onDiskFiles=2` |
| `GET /api/v1/llama/backends` | 识别既有 `llama-b12532-bin-win-cpu-x64`（legacy 目录回退），`activeCount=1` |
| `GET /api/v1/openapi.json` | 103 条路径，新路由均存在；`llama/backends`、`library`、`health` 已补元数据 |
| 页面截图（Edge headless） | `/library`：统计卡 + 资源表 + 分段筛选单行显示；`/health`：6 个分类卡片、状态横幅、修复按钮均正常 |

## 3. 前端

- `npm run build` 通过（Vite，无类型错误）。
- 新增页面：`LibraryView.vue`（扫描/校验/导入/导出/移除）、`HealthView.vue`（过滤、深度与网络开关、单条与批量修复）。
- 下载页新增模型/数据集切换；运行引擎页新增“已安装后端（并存）”表格（启用/删除/重扫）。
- 设置页新增“数据集目录”字段并接入图形化目录选择器。
- i18n：新增 118 条英文词条，中文源串为键，未命中仍回退。

## 4. 跨平台

- 新增源码均为平台中立实现（`ghc::filesystem`、SQLite、既有 `sha256File`/`diskFreeBytes`/`directoryWritable`）。
- `scripts/cross-common.sh` 以 `backend/src/*.cpp` 通配收集源文件，`offline_library.cpp`、`health_check.cpp` 自动纳入各平台构建。
- **未执行** POSIX 交叉编译：本机未安装 Zig（仅有缺 lib 的 `@zigc` npm 二进制）；从 ziglang.org 下载 82 MB 工具链在 300 s 内仅完成约 7 MB，续传 2 分钟后仍不足 10 MB，故按网络原因中止。代码路径不含平台分支，后端识别已覆盖 `.so` / `.dylib`。
- Android / iOS 仍为源码级校验，未实机验证。

## 5. 已知限制

- 离线包以绝对路径记录 `sourcePath`；跨平台/跨盘符导入需填写 `basePath` 或使用 `copy=true` 的包。
- `datasetsDir` 扫描只识别其**直接子目录**为一个数据集；嵌套层级需手动整理或重新下载到根目录。
- 健康自检的 `web-root` 依据设置中的 `webRoot` 判断，服务化部署修改静态目录后需同步设置。

## 6. 启动自动更新检查（追加）

| 项目 | 结果 |
| --- | --- |
| `testAutoUpdateSetting` | 默认开启；`PUT /settings` 可关闭/开启；非布尔值被忽略；导出包含该字段；通道与开关可同时持久化 |
| `GET /updates/startup` | 200，3.36 s；真实检测到 llama.cpp `b12532 → b12669`（`assetName=llama-b12669-bin-win-vulkan-x64.zip`，39,034,012 字节，`prerelease=true`，稳定通道回退预发布） |
| `PUT /settings {"autoCheckUpdates":false}` 后调用 | 立即返回 `enabled=false`、`items=[]`，无网络请求 |
| 应用自身 | 当前 1.0.1 无更新，未出现在 `items`，符合“无更新不显示” |
| OpenAPI | `/api/v1/updates/startup` 已登记（get） |
| 页面截图（Edge headless） | 顶部横幅显示“发现可用更新 / llama.cpp b12532 → b12669 / 稳定版 / 预发布 / 37.23 MB / 更新 / 查看发布说明”，位于端口横幅之上，布局正常 |

检测按钮触发的实际下载未执行（会替换本机 llama.cpp 运行时）；下载/应用链路沿用
已发布的更新流程与 `POST /llama/install`。
