# iOS 分发方案复核（2026-11-24）

## 1. 结论

iOS 目前有两个彼此独立的问题，不能混为一谈：

1. **签名与分发**：Apple 要求所有在 iOS 上运行的代码都必须由 Apple 账户签名。当前没有 Apple Developer Program 成员资格，也没有 Xcode 工程 / IPA，所以发不出去。
2. **运行形态**：当前 `modelforge` 是控制台服务，依赖外部 `python` / `llama-quantize` 子进程、任意绝对路径、自更新下载后执行二进制。即使签了名，这套形态也不符合 iOS 沙盒与 App Store 规则（详见第 4 节）。

**换一种分发方式只能解决问题 1；要真正上架，还必须解决问题 2。**

## 2. 分发方式对比

| 方式 | 费用 | 是否需要 Mac | 有效期 / 限制 | 适用 |
| --- | --- | --- | --- | --- |
| Personal Team（免费 Apple ID）+ Sideloadly / AltStore | 0 | **构建需要 macOS**；安装可在 Windows 用 Sideloadly | App ID、设备、描述文件 7 天过期；每台设备最多 3 个 App；iOS 16+ 需开启开发者模式 | 自己试用 / 临时演示 |
| TestFlight | $99/年 | 需要（可用 CI 的 macOS runner） | 每个构建 90 天；内部测试员 100 人；外部测试员 10,000 人；外部首个构建需过 App Review | 给少量用户稳定测试（**推荐**） |
| Ad Hoc | $99/年 | 需要 | 每年最多注册 100 台设备；无 App Store 审核 | 固定设备的小范围部署 |
| Apple Developer Enterprise Program | $299/年 | 需要 | 仅面向本组织员工，需 MDM；资格审核严格，证书被撤销风险高 | 企业内部，不推荐本项目 |
| App Store | $99/年 | 需要 | 审核 + 隐私政策 + 最低功能性（4.2）+ 中国区部分 App 需 ICP 备案 | 公开发布，需先完成原生化 |
| 欧盟 / 日本 / 巴西替代市场（AltStore PAL 等） | 需开发者账号 + 公证 | 需要 | 仅上述地区可用 | 与中国用户无关 |
| Web/PWA 指向远端服务 | 0 | 否 | 无本地模型/转换能力，需把文件传到服务器 | 与“本地优先”定位冲突 |

**Windows 上也能产出 IPA**：`xcodebuild` 与签名只能在 macOS 上运行，但不必买 Mac。GitHub 公开仓库的标准 `macos-latest` runner 免费（大规格 runner 仍收费），可在 CI 中完成 archive + 签名 + 导出 IPA；Windows 本机再用 Sideloadly 安装。需要把证书 / 描述文件放进 GitHub Actions Secrets，不要把签名材料提交进仓库。

官方来源：

- Apple Developer 账户对比：<https://developer.apple.com/support/compare-memberships/>
- TestFlight 概览：<https://developer.apple.com/help/app-store-connect/test-a-beta-version/testflight-overview>
- App Store 审核指南：<https://developer.apple.com/app-store/review/guidelines/>
- App Store Connect 中国区 App 信息（ICP 说明）：<https://developer.apple.com/cn/help/app-store-connect/reference/app-information/app-information>

## 3. 免费个人签名的实际可用性

可用，但只适合“自己装来试”：

- App ID / 设备 / 描述文件均为 7 天，过期后要重新构建、签名、安装；AltStore 可在同一 Wi-Fi 下由 AltServer 自动续签，Sideloadly 每次要重新连接。
- 每台设备最多 3 个 App（AltStore 自身也算一个），免费账户 App ID 数量也有限。
- iOS 16+ 必须打开“开发者模式”并信任证书，首次启动需要联网校验。
- 不能用于对外发布，也不能依赖推送、内购等能力。

因此对 ModelForge 而言：免费签名只能用于验证“iOS 壳是否跑得起来”，不能作为发布渠道。

## 4. 当前后端为什么装上也跑不通

| 现有实现 | iOS 限制 |
| --- | --- |
| `App::initialize` 接受任意 `--root`/`--dataDir` 绝对路径 | App 只能访问自己容器 + 用户通过 Document Picker 授权的目录；没有任意绝对路径 |
| `ConverterService` 启动 `python convert_hf_to_gguf.py`、`llama-quantize` 外部进程 | iOS 没有可随时下载/执行的工具链；App Store 指南 2.5.2 禁止“下载、安装或执行改变 App 功能的代码” |
| `LlamaRuntimeService` 从 GitHub 下载官方 release 并解压后执行 | 同上；且 iOS 不允许下载可执行代码 |
| `UpdateService` 下载并应用新版本 | iOS 上应由 TestFlight / App Store 负责更新 |
| `DllDirectory`/动态链接、`cpp-httplib` 常驻监听、WebSocket | 本地回环端口本身可行（许多 App 这么做），但后台会被挂起；需 `NSAllowsLocalNetworking`，且审核指南 2.5.4 限制后台服务用途 |
| 图形化目录选择器（枚举整机目录树） | 需换成 `UIDocumentPickerViewController` / Files App 授权 |

审核风险补充：指南 4.2 要求 App 不只是“网页套壳”。若采用 WKWebView + 本地服务，必须提供足够的原生功能（文件选择、主机发现、通知、设置、离线状态等），否则有被拒风险。

## 5. 三条可行路线

### A. 局域网瘦客户端（投入最小，推荐先做）

iOS App 只做原生壳 + WKWebView，连接用户自己的 ModelForge 主机：

- 后端继续运行在 Windows / Linux / macOS / Android 上，iOS 端不跑转换。
- 文件通过 Document Picker 上传到主机；任务、日志、模型管理走现有 REST / WebSocket。
- 需要处理：本地网络权限（`NSLocalNetworkUsageDescription`）、`NSAllowsLocalNetworking`、服务发现（mDNS 或手动输入 IP）、只绑定回环/局域网且加认证（当前后端只绑 `127.0.0.1`，需要先扩展为可选 LAN 监听 + Token 鉴权，**不能**直接放开无鉴权监听）。
- 分发：TestFlight（$99/年）或 Ad Hoc；自用可免费侧载。
- 优点：后端无需移植；缺点：必须有一台常开的主机，离开局域网不可用。

### B. 本地回环壳（复用 Web UI，但功能受限）

把 `cpp-httplib` 编进 iOS App，WKWebView 打开 `http://127.0.0.1:<port>`：

- 前端 95% 可复用，SQLite / GGUF 读取等纯 C++ 代码可复用。
- 必须砍掉：自更新、下载后执行 llama.cpp、任意路径浏览；外部工具改为进程内实现或禁用。
- 后台挂起导致长任务中断；审核上属于“本地 WebView 壳”，需补足原生功能。
- 适合免费侧载自用，不适合直接上架。

### C. 真原生移植（长期上架路线）

- llama.cpp 以静态库方式编入 App（llama.cpp 本身支持 iOS + Metal），转换/量化改为进程内 API 调用，不再 spawn 子进程。
- 数据集/模型下载改为在 App 内下载 + App 内解析，不执行下载的代码。
- 目录选择换成 Document Picker；更新交给 TestFlight / App Store。
- 分发：TestFlight → App Store；中国区按 App Store Connect 提示决定是否需要 ICP 备案与隐私政策。
- 工作量最大，但是唯一能稳定上架的路线。

## 6. 建议

1. **短期**：选 A（局域网瘦客户端）。先在 Windows 上实现“可选 LAN 监听 + Token 鉴权”，再做一个最小 iOS 壳，TestFlight 给 10 人以内试用。
2. **自用验证**：用 CI 打 IPA → Windows Sideloadly 安装，7 天续签即可，不要把它当成发布渠道。
3. **长期**：若要上架，按 C 做原生化；在原生化完成前不要提交 App Store，4.2 与 2.5.2 的拒绝风险很高。
4. 无论哪条路线，`scripts/build-ios.sh` 的“仅语法检查”在 Windows 上都无法真正通过（Zig 没有 iOS SDK 头文件），应改到 macOS runner 上执行；它不代表 iOS 可编译可用。
