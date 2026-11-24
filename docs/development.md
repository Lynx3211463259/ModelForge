# ModelForge 开发说明

## 1. 工具链

已验证：

| 组件 | 版本 |
|---|---|
| OS | Windows 11 |
| CMake | 4.4.4 |
| C++ | MinGW-w64 GCC 16.2.0，UCRT，POSIX threads |
| Node.js | 22.22.2 |
| Python | 3.13.14 |
| llama.cpp | 本地仓库版本（2026-11） |

### 版本号单一来源

仓库根目录 `VERSION` 是 ModelForge 自身版本的唯一来源。CMake 的
`project(VERSION ...)`、POSIX 交叉编译（`scripts/cross-common.sh`）、
Windows 打包（`scripts/package-windows.ps1`）和发布脚本
（`scripts/publish-release.sh`）都从该文件读取版本。发版时只改这一处，
重新构建后 `/api/v1/health` 的 `version`、设置页“当前版本”徽章、资产文件名
与 `SHA256SUMS.txt` 会保持一致；`core_tests` 也会校验编译进去的版本与
`VERSION` 文件一致。

### 预发布版本号

`VERSION` 允许带预发布后缀，例如 `1.1.0-beta.1`。CMake 的 `project(VERSION ...)`
只接受数字，因此 `CMakeLists.txt` 会取前缀 `1.1.0` 交给 `project()`，而编译进
程序的 `MODELEFORGE_VERSION`、资产文件名、GitHub 标签仍使用完整字符串。
更新检查按数字部分比较，正式版发布后预发布用户会收到升级提示。

发布脚本会依据版本号中的 `-` 自动把 GitHub Release 标记为预发布
（`1.1.0-beta.1` → prerelease），也可用 `PRERELEASE=1/0` 覆盖。

### 获取 Zig（POSIX 交叉编译）

`scripts/build-linux.sh`、`build-macos.sh`、`build-openeuler.sh`、`build-android.sh`、
`build-ios.sh` 需要 Zig。若 ziglang.org 下载缓慢，可用 PyPI 镜像获取同一版本：

```bash
pip download ziglang==0.14.1 --no-deps -d temp/zigpkg -i https://pypi.tuna.tsinghua.edu.cn/simple
python -c "import zipfile,sys;zipfile.ZipFile(sys.argv[1]).extractall('temp/zig')" temp/zigpkg/ziglang-0.14.1-py3-none-win_amd64.whl
ZIG="$PWD/temp/zig/ziglang/zig.exe" bash scripts/build-linux.sh
```

### 为什么需要支持线程的编译器

项目大量使用 `std::thread`、`std::mutex`、`std::condition_variable`。某些精简版 MinGW 的 libstdc++ 是单线程构建，连 `<mutex>` 都不提供，表现为：

```text
'std::mutex' is defined in header '<mutex>'; did you forget to '#include <mutex>'
```

这不是缺少 include，而是工具链不支持。请使用 WinLibs / MSYS2 UCRT64 / MSVC 等完整工具链。

## 2. 后端构建

```powershell
cmake -S . -B build-win -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/toolchains/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/toolchains/mingw64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/toolchains/mingw64/bin/mingw32-make.exe

cmake --build build-win -j 4
```

MinGW 目标会追加：

```text
-static -static-libgcc -static-libstdc++
```

避免发布包依赖本机工具链 DLL。

## 3. 前端构建

```powershell
cd frontend
npm install
npm run build
```

Vite 输出 `frontend/dist/`。C++ 服务通过 `webRoot` 设置挂载该目录。

前端开发模式：

```powershell
npm run dev
```

Vite 将 `/api` 代理到 `http://127.0.0.1:8080`。

## 4. Python 转换环境

```powershell
python -m venv .venv
.venv\Scripts\python.exe -m pip install -r ..\llama.cpp\requirements\requirements-convert_hf_to_gguf.txt
```

安装后的依赖：

- torch 2.11.0+cpu
- numpy 2.2.6
- transformers 4.57.6
- sentencepiece
- safetensors
- gguf
- protobuf

系统设置中的 `pythonPath` 指向 `.venv\Scripts\python.exe`。

## 5. llama.cpp 工具

以下命令在 llama.cpp 仓库内执行：

```powershell
cmake -S . -B build-modelforge -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/toolchains/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/toolchains/mingw64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/toolchains/mingw64/bin/mingw32-make.exe `
  -DLLAMA_BUILD_EXAMPLES=ON `
  -DLLAMA_BUILD_TOOLS=ON `
  -DLLAMA_BUILD_SERVER=ON `
  -DLLAMA_BUILD_TESTS=OFF `
  -DLLAMA_CURL=OFF `
  -DGGML_NATIVE=ON `
  -DCMAKE_CXX_FLAGS="-D_WIN32_WINNT=0x0A00" `
  -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"

cmake --build build-modelforge --target llama-quantize llama-cli llama-bench server -j 6
```

要点：

- `_WIN32_WINNT=0x0A00`：llama.cpp 自带 cpp-httplib 不支持 Windows 8 目标。
- `LLAMA_BUILD_SERVER=ON`：当前 llama.cpp 版本将 `llama-cli` 放在 server 条件内。
- `-static`：避免 quantize/cli 依赖工具链 DLL。

ModelForge 会自动探测：

```text
<root>/llama.cpp/build-modelforge/bin/
<root>/llama.cpp/build/bin/
<root>/tools/llama.cpp/bin/
```

## 6. 运行

```powershell
build-win\bin\modelforge.exe serve --root C:\path\to\ModelForge
```

参数：

```text
--root <dir>      ModelForge 根目录
--data <dir>      SQLite、日志、报告目录
--port <n>        仅本次启动使用的 HTTP 端口，不写入设置
--no-browser      仅本次启动不自动打开浏览器，不写入设置
```

## 7. 测试

### 单元测试

```powershell
build-win\bin\modelforge_tests.exe
```

覆盖：

- SHA-256 / SHA-1 标准向量
- WebSocket Accept 摘要
- 文件名清洗 / Windows 保留名 / `..`
- 大小格式化、单行日志、后缀判断

### 集成冒烟

```powershell
python tests\integration\smoke.py --base-url http://127.0.0.1:8080 `
  --model-path C:\...\ModelForge\models\TinyLlamaForge-64
```

### 手动真实转换验证

1. 导入模型目录。
2. 选择 F16 中间精度 + Q4_K_M。
3. 启动任务。
4. 检查日志中出现 torch/gguf 输出与 quantize 统计。
5. 下载 GGUF，用 `modelforge validate --file ...` 重新检查。
6. 如有 `llama-cli`，在 Model Tester 页面执行真实推理。

## 8. 添加新的模型架构支持

ModelForge 不硬编码架构列表。`ConverterService` 读取：

- `convert_hf_to_gguf.py`
- `conversion/*.py` 中的 `@ModelBase.register(...)`

升级 llama.cpp 后，点击设置页“检测工具链”或调用：

```text
GET /api/v1/capabilities?refresh=1
```

即可刷新支持的架构与量化类型。

## 9. 添加新的转换引擎

建议实现以下能力边界：

```cpp
detect()        // 工具路径、版本、架构、参数白名单
validate()      // 模型完整性、资源、输出路径
plan()          // 阶段命令、工作目录、预期产物
verify()        // GGUF 或目标格式验证
```

要求：

- 不通过 Shell 拼接命令。
- 动态探测支持能力，不显示未验证参数。
- 进度必须声明 measured / estimated / indeterminate。
- 错误必须结构化并保留原始日志。

## 10. 数据库迁移

当前 schema 在 `Database::migrate()` 中创建，单独存档在 `docs/schema.sql`。

新增迁移时：

1. 增加版本号与幂等 SQL。
2. 不删除用户数据。
3. 大字段继续放在文件系统，数据库只存索引与路径。
4. 更新 `docs/schema.sql`。

## 11. 代码风格

- C++17（兼容现有工程），但结构化绑定、`std::optional` 等已使用。
- 模块边界：API 不直接启动进程；任务管理器不解析 HTTP。
- 所有外部参数必须用数组传递。
- 大文件读取必须流式或只读头部。
- 日志单行化并截断，避免前端 DOM 被超长行拖垮。

## 12. 已知构建注意事项

- Windows 路径使用 `ghc::filesystem` 兼容层，原因是验证环境中的旧 GCC 8.1 标准库文件系统实现损坏。
- 新工具链下仍保留该兼容层，避免行为回退。
- `httplib.h` 针对新版 GCC 的 `std::min` 类型推导做了两处显式 `size_t` 修正。
- MinGW 下 SQLite 以 amalgamation 静态编译。
- WebSocket 是原生 socket 实现，不依赖第三方 WS 库；握手使用项目内 SHA-1 + Base64。
