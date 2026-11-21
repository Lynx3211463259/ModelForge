# ModelForge 跨平台适配说明

版本：1.0.0　|　更新日期：2026-11-21

本文档记录跨平台改造的平台相关点清单、适配方案、构建命令、依赖锁定、产物结构、验收矩阵、
已知风险与未实机验证项。核心功能、输入输出行为与使用结果在各平台保持一致：同一套 C++17 核心、
同一个 Vue 前端、同一份 SQLite 数据模型与同一套 REST/WebSocket 契约。

---

## 1. 结论

| 平台 | 状态 | 产物 | 实机验证 |
| --- | --- | --- | --- |
| Windows | ✅ 已构建 + 实机测试 | `ModelForge-1.0.0-win64.zip` | 是（Windows 11） |
| Linux | ✅ 已构建验证（交叉编译，静态 musl） | `ModelForge-1.0.0-linux-x64/arm64.tar.gz` | 否（构建+静态检查，未实机运行） |
| macOS | ✅ 已构建验证（交叉编译 Mach-O） | `ModelForge-1.0.0-macos-arm64/x64.tar.gz` | 否（未签名，未实机运行） |
| Android | ✅ 已构建验证（Termux 形态，静态 musl） | `ModelForge-1.0.0-android-arm64/x64.tar.gz` | 否 |
| openEuler | ✅ 已构建验证（静态 musl） | `ModelForge-1.0.0-openeuler-x64/arm64.tar.gz` | 否 |
| iOS | ❌ 阻塞（无 Apple SDK / 无证书 / 无后台服务部署路径） | 无（仅语法静态检查） | 否 |

关键说明：除 Windows 外，交叉产物均已完成**编译与链接**并通过魔数校验（ELF / Mach-O）、
前端产物打包与脚本静态检查，但**未在对应真机运行**。原因与风险见第 6 节。

---

## 2. 平台相关点清单

| # | 关注点 | Windows 实现 | 其他平台实现 | 适配结果 |
| --- | --- | --- | --- | --- |
| 1 | 文件系统 | `ghc::filesystem`（`u8string()` 流） | 同 | 无需改动，跨平台 |
| 2 | 路径分隔符 | `\`，`pathKey`/watch 归一化 | `/` | 归一化逻辑统一为 `pathKey()` |
| 3 | 权限模型 | 无需提权；目录写权限 `directoryWritable` | 同上 + token 文件 0600 | POSIX 凭据改为受限文件（见第 3 节） |
| 4 | 凭据加密 | DPAPI（`CryptProtectData`） | POSIX 用 `data/secrets/hf.token`（0600） | 已实现；`token_scheme` 区分 `dpapi`/`posix-file` |
| 5 | HTTPS 传输 | WinHTTP（Range/重定向/限速） | curl 子进程（`--location`/Range/`-o -` 流式） | 已实现（`http_client.cpp` POSIX 分支） |
| 6 | 进程管理 | `CreateProcessW` + Job Object | `fork` + `setpgid` + 信号组 | 既有实现 |
| 7 | 进程资源采样 | Job 对象账户（含子进程） | `/proc/<pid>/stat|status|io` | 既有实现 |
| 8 | 系统信息 | `GlobalMemoryStatusEx`、`GetSystemTimes` | `/proc/meminfo`、`/proc/stat`、`sysconf` | macOS 内存改为 `host_statistics64`（修复 `_SC_AVPHYS_PAGES` 缺失） |
| 9 | GPU 探测 | `nvidia-smi`、`vulkan-1.dll`、`DirectML.dll` | `nvidia-smi`、`libvulkan.so/dylib` | 兼容（`systemDirectory()` 按平台解析） |
| 10 | 运行时依赖 | Python venv + llama.cpp 官方资产 | 同（平台对应资产，`llama-runtime` 按 `linux/macos` 匹配） | 无需改动 |
| 11 | 服务托管 | 前台进程（无 Windows 服务） | systemd（Linux/openEuler）、launchd（macOS）、Termux 脚本（Android） | 新增服务定义 |
| 12 | 端口探测 | `winsock` bind | `socket`/`bind`（`inet_pton` 绑定实际地址） | `checkTcpPort(port, host)` 统一 |
| 13 | 构建配置 | CMake + MinGW GCC 16.2.0 | zig 0.14.0 交叉编译（musl/glibc/Mach-O） | 每平台独立脚本 |
| 14 | 打包 | `package-windows.ps1`（zip） | tar.gz（按平台/架构分离） | 每平台独立产物 |
| 15 | 签名 | 无（zip） | macOS 需 codesign+notarize；iOS 需证书 | 见第 6 节 |
| 16 | 应用图标 | `modelforge.exe` 通过 RC 内嵌多尺寸 ICO | Web UI 自带 favicon / PNG（全平台一致）；其他平台可执行文件无内嵌图标，启动器可引用 `docs/images/logo.png` | 同一源图生成 |

修复的跨平台缺陷（Windows 上被 `uint64_t == unsigned long long` 掩盖）：
- `converter.cpp` 两处 `std::max(16ull*1024ull, uint64_t)` 类型不一致 → `std::max<uint64_t>(...)`。
- `util.cpp` 的 `_SC_AVPHYS_PAGES` 在 macOS 不存在 → `host_statistics64` 分支。
- `util.cpp` 的 `inet_pton` 缺少 POSIX `<arpa/inet.h>` → 已补。
- `download_manager.cpp` 凭据函数原为 `static`，POSIX 分支需访问 `settings_` → 改为实例方法。

---

## 3. 适配方案（关键实现）

### 3.1 POSIX HTTPS：curl 子进程适配器

不使用额外的 TLS 链接库（避免各发行版 libcurl/OpenSSL ABI 差异），改用 `curl` 子进程：

- `ProcessRunner` 新增 `onData(const char*, size_t, bool)` 原始字节回调，二进制安全（不按行切分）。
- `request()`/`download()` 用 `curl --location --dump-header <tmp> --output - ...`，响应体流式读取；
  Range 续传通过 `-H Range: bytes=N-`，服务端返回 200 时自动从 0 重写；限速用同一 `SpeedLimiter`。
- curl 缺失时返回明确错误并提示安装方式（macOS/Linux/openEuler/Termux 均可用包管理器安装）。

### 3.2 POSIX 凭据存储

Hugging Face Token 不再落入 SQLite 明文，改为写入 `data/secrets/hf.token` 并以 `chmod 0600` 限制；
数据库只存 `posix-file:hf.token` 标记。Windows 保持 DPAPI 不变。

### 3.3 服务托管

- Linux / openEuler：`modelforge.service`（systemd，`ExecStart=... serve --no-browser`）。
- macOS：`com.modelforge.plist`（launchd，`~/Library/LaunchAgents`）。
- Android：Termux `run.sh`（先 `pkg install curl python` 再 `./modelforge serve`），产物 `*-android-*.tar.gz`。
- Windows：前台进程（与历史版本一致）。

---

## 4. 依赖清单（锁定版本）

| 组件 | 版本 | 说明 |
| --- | --- | --- |
| cpp-httplib | 0.20.0 | vendored `backend/third_party/httplib.h` |
| nlohmann/json | 3.11.3 | vendored `backend/third_party/json.hpp` |
| ghc::filesystem | vendored（无版本号） | `backend/third_party/ghc_filesystem.hpp` |
| SQLite | 3.53.4（amalgamation） | vendored `backend/third_party/sqlite3.c` |
| C++ 标准 | C++17 | 所有平台统一 |
| Windows 工具链 | MinGW-w64 GCC 16.2.0 + CMake ≥3.16 | 实机 |
| 交叉编译 | zig 0.14.0-dev.2456（`@ryoppippi/zig-win32-x64`） | Linux/macOS/Android/openEuler |
| Python（运行时） | ≥3.10（venv，转换脚本依赖） | 各平台自行安装 |
| curl（运行时） | 任意 7.6x+ | POSIX HTTPS 传输 |
| llama.cpp 官方资产 | `b12532`（随包 / 运行引擎下载） | 各平台对应资产 |

zig 版本约束：需要支持 `-target *-linux-musl`、`*-macos-none` 与 `zig c++`/`zig cc` 驱动；
已验证 0.14.0-dev.2456。若用系统 zig，建议 0.13.x/0.14.x。

---

## 5. 构建命令、目标架构、环境与产物结构树

### Windows（实机）

```
构建环境：Windows 11 + MinGW-w64 GCC 16.2.0 + CMake 3.16+
命令：
  scripts\build-windows.ps1            # 编译后端（CMake build-win）
  cd frontend && npm install && npm run build
  scripts\package-windows.ps1 -LlamaBackend cpu -LlamaTag b12532
目标架构：x64
产物：release\ModelForge-1.0.0-win64.zip
```

### Linux / openEuler / Android（交叉编译，已构建验证）

```
构建环境：Windows 宿主 + zig 0.14.0（ZIG 环境变量指向 zig 可执行文件）
命令：
  bash scripts/build-linux.sh          # x86_64 + aarch64（musl 静态）
  bash scripts/build-openeuler.sh      # x86_64 + aarch64（musl 静态）
  bash scripts/build-android.sh        # arm64 + x64（musl 静态，Termux）
目标架构：见各命令
产物：release\ModelForge-1.0.0-{linux,openeuler,android}-{x64,arm64}.tar.gz
```

### macOS（交叉编译，已构建验证，未签名）

```
构建环境：Windows 宿主 + zig 0.14.0
命令：bash scripts/build-macos.sh      # arm64 + x86_64（Mach-O）
目标架构：arm64 / x64
产物：release\ModelForge-1.0.0-macos-{arm64,x64}.tar.gz
```

### iOS（阻塞）

```
原因：需要 Apple SDK、Xcode、开发者证书；iOS 不支持常驻后台 HTTP 服务。
命令：bash scripts/build-ios.sh        # 仅 -fsyntax-only 静态检查，不产可运行产物
解除阻塞（macOS 上）：Xcode 工程 + SwiftUI/WKWebView 外壳内嵌本二进制 → codesign → 上架/TestFlight
```

### 产物结构树（POSIX 包示例）

```
ModelForge-1.0.0-linux-x64.tar.gz
└── linux-x64/
    ├── modelforge                 # 静态二进制
    ├── modelforge.service         # systemd（linux/openEuler）
    ├── run.sh                     # Android Termux
    ├── web/                       # index.html + assets/（前端）
    ├── docs/                      # 文档
    └── README.md / LICENSE
```

---

## 6. 验收矩阵

| 检查项 | Windows | Linux | macOS | Android | openEuler | iOS |
| --- | --- | --- | --- | --- | --- | --- |
| 编译通过 | ✅ 实机 | ✅ 交叉 | ✅ 交叉 | ✅ 交叉 | ✅ 交叉 | ❌ 阻塞 |
| 链接通过 | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| 产物魔数正确（PE/ELF/Mach-O） | ✅ PE | ✅ ELF | ✅ Mach-O | ✅ ELF | ✅ ELF | — |
| 单元测试 `core_tests` | ✅ 7/7 | 未跑（未实机） | 未跑 | 未跑 | 未跑 | — |
| `serve` 启动 + `/health` 200 | ✅ | 未跑 | 未跑 | 未跑 | 未跑 | — |
| 运行时识别（llama status） | ✅ b12532/CPU | 未跑 | 未跑 | 未跑 | 未跑 | — |
| 转换能力（conversionReady） | ✅ | 未跑 | 未跑 | 未跑 | 未跑 | — |
| 端口占用自动回退 | ✅ 双实例实测 | 未跑（同源逻辑） | 未跑 | 未跑 | 未跑 | — |
| HTTPS（下载/HF/更新） | ✅ WinHTTP | 未跑（curl 适配器，静态检查） | 未跑 | 未跑 | 未跑 | — |
| 前端打包进产物 | ✅ | ✅ | ✅ | ✅ | ✅ | — |
“未跑”≠失败：指在非 Windows 宿主上无法执行该平台二进制，仅完成编译/链接与静态检查。

---

## 7. 已知风险与未实机验证项

1. **非 Windows 平台未实机运行**：交叉产物通过编译/链接/魔数校验，但以下行为未在真机确认：
   POSIX HTTPS（curl 适配器）的 Range/重定向/限速细节、launchd/systemd 服务定义、
   macOS 内存统计、Android Termux 下文件系统与端口权限。
   建议在目标平台执行 `./modelforge serve` → `curl http://127.0.0.1:PORT/api/v1/health`
   → 「运行引擎」安装对应平台 llama.cpp 资产 → 一次真实转换，作为上线前复核。
2. **macOS 未签名/未公证**：首次运行会触发 Gatekeeper 拦截，需 `codesign --deep --force --sign "Developer ID Application: ..."`，
   再用 `xcrun notarytool submit --wait` 与 `xcrun stapler staple`。
3. **iOS 无法落地为可分发产物**：非工具链问题，而是平台策略（无后台常驻服务 + 必须签名上架）所致。
4. **Android 采用 Termux 控制台形态**：普通 Android 应用无法常驻 HTTP 服务，
   因此后端以 Termux 内运行的静态二进制提供。
5. **静态 musl 二进制体积较大**（~40–60 MB，含 libc++ 静态链接 + httplib）：
   已在链接加 `-s` 去除符号；如需进一步压缩可改为动态链接 glibc（Linux/openEuler 可换 `-target *-linux-gnu`）。
6. **交叉编译工具链版本敏感**：`zig 0.14.0-dev` 为开发版；正式发布建议锁 0.14.0 稳定版并重跑全部目标。
7. **全局下载限速在 POSIX 上为逐进程执行**：curl 适配器对每个下载进程施加限速，跨文件的总带宽语义
   与 Windows 的共享令牌桶不完全一致（单文件限速一致）。已注明，未改变文件最终结果与校验逻辑。

---

## 8. 复现步骤（在对应平台自测）

```bash
# Linux / openEuler
tar xzf ModelForge-1.0.0-linux-x64.tar.gz && cd linux-x64
./modelforge serve --no-browser &  curl http://127.0.0.1:8080/api/v1/health

# macOS（先签名）
xattr -dr com.apple.quarantine ./modelforge   # 或完整 codesign + notarize
./modelforge serve &  curl http://127.0.0.1:8080/api/v1/health

# Android（Termux）
pkg install curl python
tar xzf ModelForge-1.0.0-android-arm64.tar.gz && cd android-arm64 && bash run.sh
```
