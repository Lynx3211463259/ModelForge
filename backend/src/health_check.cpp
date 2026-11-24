#include "modelforge/health_check.hpp"

#include "modelforge/hf_client.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <chrono>

namespace mf {

namespace {

bool directoryReady(const fs::path& dir, std::string& detail) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        detail = "目录不存在: " + pathToUtf8(dir);
        return false;
    }
    if (!fs::is_directory(dir, ec)) {
        detail = "路径不是目录: " + pathToUtf8(dir);
        return false;
    }
    if (!directoryWritable(dir)) {
        detail = "目录不可写: " + pathToUtf8(dir);
        return false;
    }
    detail = pathToUtf8(dir);
    return true;
}

} // namespace

HealthCheckService::HealthCheckService(Database& db, SettingsService& settings, ConverterService& converter,
                                       LlamaRuntimeService& llama, OfflineLibrary& library,
                                       std::function<std::string()> tokenProvider)
    : db_(db), settings_(settings), converter_(converter), llama_(llama), library_(library),
      tokenProvider_(std::move(tokenProvider)) {}

json HealthCheckService::makeItem(const std::string& id, const std::string& category, const std::string& title,
                                  const std::string& status, const std::string& detail, const std::string& hint,
                                  const std::string& fix) {
    return json{{"id", id}, {"category", category}, {"title", title}, {"status", status},
                {"detail", detail}, {"hint", hint}, {"fix", fix}, {"fixable", !fix.empty()}};
}

json HealthCheckService::buildReport(const json& items) {
    int ok = 0, warn = 0, error = 0;
    for (const auto& item : items) {
        const std::string status = item.value("status", "ok");
        if (status == "error") ++error;
        else if (status == "warn") ++warn;
        else ++ok;
    }
    const std::string overall = error > 0 ? "error" : (warn > 0 ? "warn" : "ok");
    return json{{"status", overall}, {"checkedAt", isoNow()},
                {"summary", json{{"total", static_cast<int>(items.size())}, {"ok", ok}, {"warn", warn}, {"error", error}}},
                {"items", items},
                {"environment", json{{"os", getOsName()}, {"host", getHostName()},
                                     {"version", MODELEFORGE_VERSION},
                                     {"memoryTotal", totalSystemMemory()},
                                     {"memoryAvailable", availableSystemMemory()}}}};
}

json HealthCheckService::checkAll(bool deep, bool network) {
    const AppSettings s = settings_.get();
    std::error_code ec;
    json items = json::array();
    auto add = [&](json item) { items.push_back(std::move(item)); };

    // ---- Database ----
    {
        const auto rows = db_.query("PRAGMA quick_check");
        const std::string result = rows.empty() ? "" : rows[0].value("quick_check", "");
        if (result == "ok") {
            add(makeItem("database", "database", "SQLite 数据库", "ok", "完整性检查通过（PRAGMA quick_check）", ""));
        } else {
            add(makeItem("database", "database", "SQLite 数据库", "error",
                         result.empty() ? "数据库未初始化或无法读取" : ("完整性检查失败: " + result),
                         "备份 data/modelforge.db 后重启程序；若损坏请从备份恢复。", ""));
        }
        const auto writeTest = db_.query("SELECT COUNT(*) AS n FROM app_settings");
        if (writeTest.empty()) {
            add(makeItem("database-write", "database", "数据库写入", "warn", "无法读取 app_settings 表",
                         "确认数据目录权限，必要时重建数据库。", ""));
        }
    }

    // ---- Directories ----
    struct DirCheck { const char* id; const char* title; fs::path dir; };
    const DirCheck dirs[] = {
        {"dir-models", "模型目录", settings_.modelsDir()},
        {"dir-datasets", "数据集目录", settings_.datasetsDir()},
        {"dir-output", "输出目录", settings_.outputDir()},
        {"dir-temp", "临时目录", settings_.tempDir()},
        {"dir-data", "数据目录", settings_.dataDir()}
    };
    for (const auto& check : dirs) {
        std::string detail;
        const bool ok = directoryReady(check.dir, detail);
        add(makeItem(check.id, "storage", check.title, ok ? "ok" : "error", detail,
                     ok ? "" : "点击修复创建目录，或在设置中更换到可写路径。", ok ? "" : "ensure_dirs"));
    }

    // ---- Disk space ----
    {
        const uint64_t freeBytes = diskFreeBytes(settings_.modelsDir());
        const uint64_t totalBytes = diskTotalBytes(settings_.modelsDir());
        std::string status = "ok";
        std::string hint;
        if (totalBytes > 0 && freeBytes < 2ull * GB) {
            status = freeBytes < 512ull * MB ? "error" : "warn";
            hint = "模型转换与下载需要可观空间；请清理磁盘或更换目录。";
        }
        add(makeItem("disk-space", "storage", "磁盘剩余空间", status,
                     "可用 " + humanBytes(freeBytes) + " / 共 " + humanBytes(totalBytes), hint));
    }

    // ---- Python toolchain ----
    const EngineCapabilities caps = converter_.capabilities(deep);
    add(makeItem("python", "toolchain", "Python 解释器", caps.pythonAvailable ? "ok" : "error",
                 caps.pythonAvailable ? caps.pythonVersion : ("未找到 Python: " + s.pythonPath),
                 caps.pythonAvailable ? "" : "安装 Python 3.10+ 并在设置中填写可执行文件路径。", ""));
    add(makeItem("python-deps", "toolchain", "Python 转换依赖", caps.pythonDependenciesReady ? "ok" : "error",
                 caps.pythonDependenciesReady
                     ? "torch / transformers / sentencepiece 等依赖可用"
                     : (caps.pythonDependencyError.empty() ? "缺少转换所需依赖" : caps.pythonDependencyError),
                 caps.pythonDependenciesReady ? "" : "在 Python 环境执行: pip install -r requirements.txt（llama.cpp 源码目录）。", ""));
    add(makeItem("convert-script", "toolchain", "转换脚本 convert_hf_to_gguf.py",
                 caps.convertScriptAvailable ? "ok" : "error",
                 caps.convertScriptAvailable ? caps.convertScriptPath : "未找到转换脚本",
                 caps.convertScriptAvailable ? "" : "在运行引擎页安装官方源码包，或在设置中指定脚本路径。", ""));

    // ---- llama.cpp backends ----
    const json backendList = llama_.backends();
    {
        const int count = backendList.value("count", 0);
        const std::string activeId = backendList.value("activeId", "");
        json names = json::array();
        for (const auto& item : backendList["items"]) {
            names.push_back(item.value("id", "") + " [" + item.value("backend", "") + "]");
        }
        add(makeItem("llama-backends", "runtime", "已安装的 llama.cpp 后端",
                     count > 0 ? "ok" : "error",
                     count > 0 ? (std::to_string(count) + " 个后端可用；当前: " +
                                  (activeId.empty() ? std::string("未标记") : activeId))
                               : "未发现任何可用的 llama.cpp 后端",
                     count > 0 ? "" : "在运行引擎页安装 CPU 或 CUDA 官方发行版。",
                     "refresh_backends"));
    }
    const std::vector<std::pair<std::string, std::string>> tools{
        {"tool-quantize", s.quantizePath}, {"tool-cli", s.llamaCliPath},
        {"tool-server", s.llamaServerPath}, {"tool-bench", s.llamaBenchPath},
        {"tool-ppl", s.llamaPplPath}
    };
    for (const auto& tool : tools) {
        const bool exists = !tool.second.empty() && fs::is_regular_file(pathFromUtf8(tool.second), ec);
        add(makeItem(tool.first, "runtime", tool.first == "tool-quantize" ? "量化工具 llama-quantize" :
                                                                    tool.first == "tool-cli" ? "推理工具 llama-cli" :
                                                                    tool.first == "tool-server" ? "服务端 llama-server" :
                                                                    tool.first == "tool-bench" ? "基准测试 llama-bench" :
                                                                                                  "困惑度 llama-perplexity",
                     exists ? "ok" : "warn",
                     exists ? tool.second : (tool.second.empty() ? "未配置路径" : ("文件不存在: " + tool.second)),
                     exists ? "" : "启用一个已安装后端，或执行工具路径自动修复。", exists ? "" : "repair_tools"));
    }

    // ---- Offline library ----
    {
        const json stats = library_.stats();
        const int total = stats.value("total", 0);
        const int missing = stats.value("missing", 0);
        std::string status = missing > 0 ? "warn" : "ok";
        if (total == 0) status = "warn";
        add(makeItem("offline-library", "library", "离线资源库",
                     status,
                     total == 0 ? "尚未索引任何离线资源"
                                : (std::to_string(total) + " 项资源，磁盘占用 " + humanBytes(stats.value("bytes", 0ull)) +
                                   (missing > 0 ? "，" + std::to_string(missing) + " 项路径已失效" : "")),
                     status == "ok" ? "" : "运行一次资源库扫描，或移除失效条目。", "rescan_library"));
    }

    // ---- Settings sanity ----
    {
        const bool portOk = s.port >= 1024 && s.port <= 65535;
        const bool bindOk = s.bindAddress == "127.0.0.1" || s.bindAddress == "localhost";
        add(makeItem("settings-network", "config", "监听配置",
                     (portOk && bindOk) ? "ok" : "error",
                     "端口 " + std::to_string(s.port) + "，地址 " + s.bindAddress,
                     (portOk && bindOk) ? "" : "端口需在 1024-65535 之间，且仅允许本机回环地址。", ""));
        const bool webOk = !s.webRoot.empty() && fs::is_directory(pathFromUtf8(s.webRoot), ec);
        add(makeItem("web-root", "config", "前端静态资源", webOk ? "ok" : "warn",
                     webOk ? s.webRoot : ("目录不存在: " + (s.webRoot.empty() ? std::string("(空)") : s.webRoot)),
                     webOk ? "" : "重新构建前端（npm run build）或重新解压发行包。", ""));
    }

    // ---- Optional network probe ----
    if (network) {
        const std::string token = tokenProvider_ ? tokenProvider_() : std::string();
        if (token.empty()) {
            add(makeItem("hf-token", "network", "Hugging Face Token", "warn", "未配置 Token（公开仓库仍可下载）",
                         "私有/受限仓库需要在下载中心配置具备 read 权限的 Token。", ""));
            add(makeItem("hf-network", "network", "Hugging Face 连通性", "warn", "跳过：未配置 Token 时不发起网络探测",
                         "配置 Token 后可在这里验证连通性。", ""));
        } else {
            const HfTokenInfo info = HfClient::validateToken(token);
            add(makeItem("hf-token", "network", "Hugging Face Token", info.valid ? "ok" : "error",
                         info.valid ? ("已认证: " + (info.username.empty() ? std::string("(未知用户)") : info.username))
                                    : (info.error.empty() ? "Token 校验失败" : info.error),
                         info.valid ? "" : "Token 可能过期或权限不足，请重新生成。", ""));
        }
    }

    json report = buildReport(items);
    report["deep"] = deep;
    report["networkChecked"] = network;
    return report;
}

json HealthCheckService::run(bool deep, bool network) {
    const int64_t started = nowMs();
    json report = checkAll(deep, network);
    report["elapsedMs"] = nowMs() - started;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = report;
    }
    return report;
}

json HealthCheckService::fix(const std::string& id, std::string& error) {
    const std::string action = trim(id);
    std::string message;
    if (action == "ensure_dirs") {
        const fs::path dirs[] = {settings_.modelsDir(), settings_.datasetsDir(), settings_.outputDir(),
                                 settings_.tempDir(), settings_.logsDir()};
        int created = 0;
        for (const auto& dir : dirs) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) {
                if (ensureDirectory(dir, message)) ++created;
                else {
                    error = "创建目录失败: " + message;
                    return json::object();
                }
            }
        }
        message = "已确认 " + std::to_string(created) + " 个目录";
    } else if (action == "repair_tools") {
        // load() re-detects missing tool paths from the official release layout.
        settings_.load();
        message = "已重新检测工具路径";
    } else if (action == "refresh_backends") {
        const json refreshed = llama_.refreshBackends();
        message = "已重新扫描后端目录，共 " + std::to_string(refreshed.value("count", 0)) + " 个";
    } else if (action == "rescan_library") {
        const json stats = library_.rescan(true);
        message = "已索引 " + std::to_string(stats.value("indexed", 0)) + " 项资源";
    } else if (!action.empty() && action != "all") {
        error = "不支持的修复动作: " + action;
        return json::object();
    }

    json report = checkAll(false, false);
    json items = report["items"];
    json matched = json::object();
    for (const auto& item : items) {
        if (item.value("id", "") == action) {
            matched = item;
            break;
        }
    }
    Logger::instance().info("[health] 执行修复 " + action + ": " + message);
    return json{{"action", action}, {"message", message}, {"item", matched}, {"report", report}};
}

json HealthCheckService::summary() {
    json cached;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached = last_;
    }
    if (!cached.is_object() || cached.empty()) cached = run(false, false);
    return json{{"status", cached.value("status", "ok")},
                {"checkedAt", cached.value("checkedAt", "")},
                {"summary", cached.value("summary", json::object())}};
}

} // namespace mf
