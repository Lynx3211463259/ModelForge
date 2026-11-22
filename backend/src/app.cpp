#include "modelforge/app.hpp"

#include "modelforge/gguf_reader.hpp"
#include "modelforge/hf_client.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/model_scanner.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace mf {

namespace {

bool sameRuntimePath(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    std::string a = pathToUtf8(fs::absolute(left, ec).lexically_normal());
    std::string b = pathToUtf8(fs::absolute(right, ec).lexically_normal());
#ifdef _WIN32
    a = toLower(a);
    b = toLower(b);
#endif
    return a == b;
}

bool copyRuntimeTree(const fs::path& source, const fs::path& target, std::string& error) {
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    for (fs::recursive_directory_iterator it(source, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        const fs::path relative = it->path().lexically_relative(source);
        if (relative.empty()) continue;
        const fs::path destination = target / relative;
        if (it->is_directory(ec)) {
            fs::create_directories(destination, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(destination.parent_path(), ec);
            if (!ec) fs::copy_file(it->path(), destination, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            error = ec.message();
            return false;
        }
    }
    return true;
}

} // namespace

App::App(AppOptions options) : options_(std::move(options)) {}
App::~App() { stop(); }

fs::path App::detectRoot() const {
    std::vector<fs::path> candidates;
    std::error_code ec;
    candidates.push_back(fs::current_path(ec));
    const fs::path exe = pathFromUtf8(getExecutablePath());
    const fs::path exeDir = exe.parent_path();
    candidates.push_back(exeDir);
    candidates.push_back(exeDir.parent_path());
    candidates.push_back(exeDir.parent_path().parent_path());
    candidates.push_back(exeDir.parent_path().parent_path().parent_path());
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (fs::exists(candidate / "backend" / "include", ec) && fs::exists(candidate / "CMakeLists.txt", ec)) {
            return fs::absolute(candidate, ec);
        }
    }
    // A portable build has no source markers; keep the install root next to the executable
    // instead of walking up to an arbitrary parent directory.
    return fs::absolute(exeDir, ec);
}

bool App::initialize(std::string& error) {
    if (initialized_) return true;
    if (options_.root.empty()) options_.root = detectRoot();
    options_.root = fs::absolute(options_.root);
    std::error_code ec;
    const fs::path exeDir = fs::absolute(pathFromUtf8(getExecutablePath()).parent_path(), ec);
    const fs::path runtimeBase = exeDir.empty() ? options_.root : exeDir;
    const fs::path dataDir = options_.dataDir.empty() ? (runtimeBase / "data") : fs::absolute(options_.dataDir);
    bool migratedLegacyData = false;
    std::string migrationError;
    const fs::path legacyData = options_.root / "data";
    if (options_.dataDir.empty() && !sameRuntimePath(legacyData, dataDir) &&
        !fs::exists(dataDir / "modelforge.db", ec) && fs::exists(legacyData / "modelforge.db", ec)) {
        migratedLegacyData = copyRuntimeTree(legacyData, dataDir, migrationError);
    }
    fs::create_directories(dataDir, ec);
    fs::create_directories(dataDir / "logs", ec);
    fs::create_directories(dataDir / "reports", ec);
    fs::create_directories(runtimeBase / "models", ec);
    fs::create_directories(runtimeBase / "output", ec);
    fs::create_directories(runtimeBase / "temp", ec);
    Logger::instance().setLogDirectory(dataDir / "logs");

    db_.reset(new Database());
    if (!db_->open(pathToUtf8(dataDir / "modelforge.db"), error)) return false;
    settings_.reset(new SettingsService(*db_, options_.root, dataDir, runtimeBase));
    if (options_.portOverride > 0) {
        std::string settingsError;
        settings_->update(json{{"port", options_.portOverride}}, settingsError);
        if (!settingsError.empty()) {
            error = settingsError;
            return false;
        }
    }
    if (options_.noBrowser) {
        std::string settingsError;
        settings_->update(json{{"autoOpenBrowser", false}}, settingsError);
    }
    converter_.reset(new ConverterService(*settings_));
    events_.reset(new EventHub());
    tasks_.reset(new TaskManager(*db_, *settings_, *converter_, *events_));
    system_.reset(new SystemService(*db_, *settings_, *converter_, *tasks_));
    downloads_.reset(new DownloadManager(*db_, *settings_, *system_, *events_));
    llama_.reset(new LlamaRuntimeService(*settings_));
    quality_.reset(new QualityService(*db_, *settings_, *system_, *events_));
    watch_.reset(new WatchService(*db_, *settings_, *system_, *tasks_, *events_));
    ws_.reset(new WsServer());
    update_.reset(new UpdateService(runtimeBase, runtimeBase / "temp"));
    api_.reset(new ApiServer(*settings_, *converter_, *tasks_, *system_, *downloads_, *llama_, *quality_, *watch_,
                             *events_, *ws_, *update_));
    initialized_ = true;
    Logger::instance().info("ModelForge " + std::string(MODELEFORGE_VERSION) + " initialized at " + pathToUtf8(options_.root));
    Logger::instance().info("Runtime data directory: " + pathToUtf8(runtimeBase));
    if (migratedLegacyData) {
        Logger::instance().info("Migrated legacy runtime data from " + pathToUtf8(legacyData) + " to " + pathToUtf8(dataDir));
    } else if (!migrationError.empty()) {
        Logger::instance().warn("Legacy runtime data migration failed: " + migrationError);
    }
    return true;
}

int App::resolveListenPort(int preferred, std::string& outcome) const {
    return resolveListenPort(preferred, "127.0.0.1", outcome);
}

int App::resolveListenPort(int preferred, const std::string& host, std::string& outcome) const {
    outcome = "free";
    const int probe = checkTcpPort(preferred, host);
    if (probe < 0) {
        // Cannot tell (firewall, socket API failure): let the real bind attempt decide.
        outcome = "unknown";
        return preferred;
    }
    if (probe == 1) return preferred;
    const int lastCandidate = std::min(65535, preferred + 50);
    for (int candidate = preferred + 1; candidate <= lastCandidate; ++candidate) {
        if (checkTcpPort(candidate, host) == 1) {
            outcome = "moved";
            return candidate;
        }
    }
    outcome = "exhausted";
    return -1;
}

void App::openBrowser(int port) {
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    Logger::instance().info("Opening browser: " + url);
#ifdef _WIN32
    const std::wstring wideUrl(url.begin(), url.end());
    ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::system(("open '" + url + "' >/dev/null 2>&1 &").c_str());
#else
    std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str());
#endif
}

int App::runServer() {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "ModelForge 启动失败: " << error << std::endl;
        return 1;
    }
    if (!api_->prepare(error)) {
        std::cerr << "API 初始化失败: " << error << std::endl;
        return 1;
    }
    AppSettings s = settings_->get();

    // Port auto-fallback: probe the configured port before binding. When it is taken,
    // scan upwards for a free one and persist the choice so the UI, health endpoint
    // and CLI all report the port that is actually used.
    std::string portOutcome;
    const int preferredPort = s.port;
    const int listenPort = resolveListenPort(preferredPort, s.bindAddress, portOutcome);
    if (listenPort <= 0) {
        error = "端口 " + std::to_string(preferredPort) + " 及其后 50 个候选端口均被占用，无法启动 Web 服务。"
                "请关闭占用该端口的进程，或用 --port 指定其他起始端口。";
        Logger::instance().error(error);
        std::cerr << error << std::endl;
        stop();
        return 2;
    }
    if (listenPort != preferredPort) {
        std::string settingsError;
        settings_->update(json{{"port", listenPort}}, settingsError);
        Logger::instance().warn("端口 " + std::to_string(preferredPort) + " 已被占用，自动切换到 " +
                                std::to_string(listenPort) +
                                (settingsError.empty() ? "；新端口已写回设置。"
                                                       : "；写回设置失败：" + settingsError));
        s = settings_->get();
    } else if (portOutcome == "unknown") {
        Logger::instance().warn("端口 " + std::to_string(preferredPort) +
                                " 的占用状态检测失败，将尝试直接绑定，失败时再自动回退。");
    }

    // WebSocket listens next to the API port; same availability probe, and it degrades
    // to SSE/polling when no nearby port is free instead of failing startup.
    int wsPort = 0;
    for (int candidate : {s.port + 1, s.port - 1}) {
        if (candidate >= 1024 && candidate <= 65535 && checkTcpPort(candidate, s.bindAddress) == 1) { wsPort = candidate; break; }
    }
    for (int delta = 2; wsPort == 0 && delta <= 20; ++delta) {
        const int candidate = s.port + delta;
        if (candidate <= 65535 && checkTcpPort(candidate, s.bindAddress) == 1) wsPort = candidate;
    }
    if (wsPort == 0) {
        Logger::instance().warn("未找到可用的 WebSocket 端口；Web UI 将使用 SSE 与轮询作为实时通道。");
    } else {
        std::string wsError;
        if (!ws_->start("127.0.0.1", wsPort, wsError)) {
            Logger::instance().warn(wsError + "；Web UI 将使用 SSE 与轮询作为实时通道。");
        } else {
            Logger::instance().info("WebSocket listening on ws://127.0.0.1:" + std::to_string(ws_->port()) + "/ws");
            events_->setObserver([this](const json& event) { ws_->broadcast(event); });
        }
    }

    tasks_->start();
    downloads_->start();
    watch_->start();
    if (s.autoOpenBrowser && !options_.noBrowser) {
        std::thread([this, port = s.port] { openBrowser(port); }).detach();
    }
    Logger::instance().info("ModelForge Web UI: http://" + s.bindAddress + ":" + std::to_string(s.port));

    // The probe and the bind are not atomic: if something grabs the port in between,
    // fall back once more instead of giving up.
    bool listened = api_->listen(error);
    if (!listened) {
        Logger::instance().warn("绑定 " + std::to_string(s.port) + " 失败（" + error + "），尝试其他端口。");
        std::string retryOutcome;
        int retryPort = resolveListenPort(s.port + 1, s.bindAddress, retryOutcome);
        if (retryPort > 0) {
            std::string settingsError;
            settings_->update(json{{"port", retryPort}}, settingsError);
            Logger::instance().warn("自动切换到端口 " + std::to_string(retryPort) +
                                    (settingsError.empty() ? "；新端口已写回设置。"
                                                           : "；写回设置失败：" + settingsError));
            s = settings_->get();
            error.clear();
            listened = api_->listen(error);
        }
    }
    if (!listened) {
        Logger::instance().error(error);
        std::cerr << error << std::endl;
    }
    stop();
    if (!listened) return 2;
    return 0;
}

int App::runScan(const std::string& input, bool jsonOutput) {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "初始化失败: " << error << std::endl;
        return 1;
    }
    const ModelScanResult scan = ModelScanner::scan(pathFromUtf8(input));
    const json report = ModelScanner::buildInspectionReport(scan);
    if (jsonOutput) {
        std::cout << report.dump(2) << std::endl;
    } else {
        std::cout << "模型: " << scan.name << "\n"
                  << "路径: " << scan.path << "\n"
                  << "架构: " << scan.architecture << " (" << scan.architectureFamily << ")\n"
                  << "参数: " << scan.parameterCount << "\n"
                  << "精度: " << scan.dtype << "\n"
                  << "大小: " << humanBytes(scan.totalSize) << "\n"
                  << "分片: " << scan.shardCount << "\n"
                  << "张量: " << scan.tensorCount << "\n"
                  << "完整: " << (scan.complete ? "是" : "否") << "\n";
        for (const auto& item : scan.errors) std::cout << "[错误] " << item << "\n";
        for (const auto& item : scan.warnings) std::cout << "[警告] " << item << "\n";
    }
    return scan.errors.empty() ? 0 : 2;
}

int App::runValidate(const std::string& file, bool jsonOutput) {
    GgufInfo info = GgufReader::read(pathFromUtf8(file), true);
    if (jsonOutput) {
        std::cout << info.toJson().dump(2) << std::endl;
    } else {
        std::cout << "文件: " << file << "\n"
                  << "有效: " << (info.valid ? "是" : "否") << "\n"
                  << "GGUF 版本: " << info.version << "\n"
                  << "架构: " << info.architecture << "\n"
                  << "量化: " << info.quantType << "\n"
                  << "张量: " << info.tensorCount << "\n"
                  << "文件大小: " << humanBytes(info.fileSize) << "\n";
        for (const auto& item : info.errors) std::cout << "[错误] " << item << "\n";
    }
    return info.valid ? 0 : 2;
}

int App::runConvert(const std::string& input, const std::string& outputDir, const std::string& outputName,
                    const std::string& outType, const std::string& quant, int threads, bool overwrite) {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "初始化失败: " << error << std::endl;
        return 1;
    }
    tasks_->start();
    ConversionParams params;
    params.inputPath = pathToUtf8(fs::absolute(pathFromUtf8(input)));
    params.outputDir = pathToUtf8(fs::absolute(pathFromUtf8(outputDir)));
    params.outputName = outputName.empty() ? "model" : outputName;
    params.outType = outType.empty() ? "f16" : outType;
    params.quantType = quant;
    params.threads = threads > 0 ? threads : settings_->get().threads;
    params.overwrite = overwrite;
    params.verify = true;
    params.computeSha256 = true;
    std::string taskId, code;
    if (!tasks_->createTask(params, taskId, code, error)) {
        std::cerr << "无法创建任务 [" << code << "]: " << error << std::endl;
        tasks_->stop();
        return 1;
    }
    std::cout << "任务已创建: " << taskId << std::endl;
    int64_t afterSeq = 0;
    int exitCode = 1;
    while (true) {
        const json logs = tasks_->logs(taskId, static_cast<int>(afterSeq), 1000, "");
        for (const auto& item : logs.value("items", json::array())) {
            afterSeq = std::max<int64_t>(afterSeq, item.value("seq", 0ll));
            std::cout << "[" << item.value("level", "INFO") << "] " << item.value("message", "") << std::endl;
        }
        const json task = tasks_->getTask(taskId);
        if (!task.is_object()) { std::cerr << "任务记录丢失" << std::endl; break; }
        const std::string status = task.value("status", "");
        if (status == "SUCCEEDED") {
            std::cout << "转换成功: " << task.value("outputPath", "") << " (" << humanBytes(task.value("outputSize", 0ull)) << ")"
                      << std::endl;
            exitCode = 0;
            break;
        }
        if (status == "FAILED" || status == "CANCELED") {
            std::cerr << "任务" << (status == "FAILED" ? "失败" : "已取消") << " [" << task.value("errorCode", "") << "]: "
                      << task.value("errorMessage", "") << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    tasks_->stop();
    return exitCode;
}

namespace {

bool wildcardMatch(const std::string& pattern, const std::string& value) {
    size_t p = 0, v = 0, star = std::string::npos, mark = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = v;
        } else if (star != std::string::npos) {
            p = star + 1;
            v = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

} // namespace

int App::runDownloadHf(const std::string& repo, const std::string& revision, const std::string& output,
                       int concurrency, uint64_t maxSpeed, const std::string& files, const std::string& includePattern,
                       const std::string& excludePattern, bool weightsOnly, const std::string& token) {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "初始化失败: " << error << std::endl;
        return 1;
    }
    if (!token.empty()) {
        std::string tokenError;
        downloads_->setToken(token, tokenError);
        if (!tokenError.empty()) {
            std::cerr << "Token 无效: " << tokenError << std::endl;
            return 1;
        }
    }
    const HfResolveResult resolved = HfClient::resolve(repo, revision, downloads_->token(), 30000);
    if (!resolved.ok) {
        std::cerr << "仓库解析失败 [" << resolved.code << "]: " << resolved.error << std::endl;
        if (!resolved.suggestion.empty()) std::cerr << "建议: " << resolved.suggestion << std::endl;
        return 2;
    }
    std::set<std::string> explicitFiles;
    for (const auto& item : split(files, ',')) {
        const std::string trimmed = trim(item);
        if (!trimmed.empty()) explicitFiles.insert(trimmed);
    }
    json selected = json::array();
    json sizes = json::object();
    uint64_t selectedBytes = 0;
    for (const auto& file : resolved.repository.files) {
        bool take = false;
        if (!explicitFiles.empty()) {
            take = explicitFiles.count(file.path) > 0;
        } else if (weightsOnly) {
            take = file.category == "safetensors" || file.category == "pytorch" || file.category == "gguf" ||
                   file.category == "index";
        } else {
            take = true;
        }
        if (take && !includePattern.empty() && !wildcardMatch(includePattern, file.path)) take = false;
        if (take && !excludePattern.empty() && wildcardMatch(excludePattern, file.path)) take = false;
        if (!take) continue;
        selected.push_back(json{{"path", file.path}, {"size", file.size}, {"sha256", file.sha256}});
        sizes[file.path] = json{{"size", file.size}, {"sha256", file.sha256}};
        selectedBytes += file.size;
    }
    if (selected.empty()) {
        std::cerr << "没有匹配到要下载的文件" << std::endl;
        return 2;
    }
    fs::path destination = output.empty()
        ? (settings_->modelsDir() / pathFromUtf8(resolved.repository.name))
        : fs::absolute(pathFromUtf8(output));
    DownloadRequest request;
    request.repoId = resolved.repository.repoId;
    request.revision = resolved.repository.revision;
    request.commitHash = resolved.repository.commitHash;
    request.destination = pathToUtf8(destination);
    for (const auto& entry : selected) request.files.push_back(entry.value("path", ""));
    request.concurrency = concurrency > 0 ? concurrency : 4;
    request.maxSpeed = maxSpeed;
    request.selection = json{{"sizes", sizes},
                             {"repository", json{{"repoId", resolved.repository.repoId},
                                                 {"revision", resolved.repository.revision},
                                                 {"commitHash", resolved.repository.commitHash},
                                                 {"license", resolved.repository.license},
                                                 {"gated", resolved.repository.gated}}}};
    std::string taskId, code;
    if (!downloads_->createTask(request, taskId, code, error)) {
        std::cerr << "创建下载任务失败 [" << code << "]: " << error << std::endl;
        return 2;
    }
    std::cout << "下载任务: " << taskId << "\n"
              << "仓库: " << resolved.repository.repoId << "@" << resolved.repository.revision
              << " (" << resolved.repository.commitHash.substr(0, 12) << ")\n"
              << "目录: " << pathToUtf8(destination) << "\n"
              << "文件: " << selected.size() << " 个，共 " << humanBytes(selectedBytes) << "\n"
              << "并发: " << request.concurrency << "\n";
    downloads_->start();
    int64_t lastLogSeq = 0;
    int exitCode = 1;
    int64_t lastPrint = 0;
    while (true) {
        const json task = downloads_->getTask(taskId, false);
        if (task.empty()) {
            std::cerr << "任务记录丢失" << std::endl;
            break;
        }
        const std::string status = task.value("status", "");
        const int64_t now = nowMs();
        if (now - lastPrint > 900) {
            lastPrint = now;
            std::cout << "[" << task.value("statusLabel", status) << "] "
                      << task.value("progress", 0) << "%  "
                      << humanBytes(task.value("downloadedBytes", 0ull)) << " / "
                      << humanBytes(task.value("totalBytes", 0ull)) << "  "
                      << humanBytes(static_cast<uint64_t>(task.value("speed", 0.0))) << "/s"
                      << (task.value("currentFiles", "").empty() ? "" : ("  ← " + task.value("currentFiles", "")))
                      << std::endl;
        }
        const json logs = downloads_->logs(taskId, lastLogSeq, 200);
        for (const auto& item : logs.value("items", json::array())) {
            lastLogSeq = std::max<int64_t>(lastLogSeq, item.value("seq", 0ll));
            if (item.value("level", "") != "INFO") {
                std::cout << "[" << item.value("level", "") << "] " << item.value("message", "") << std::endl;
            }
        }
        if (status == "completed") {
            const json result = task.value("result", json::object());
            std::cout << "下载完成: " << task.value("destination", "") << "，" << humanBytes(task.value("totalBytes", 0ull))
                      << "\n" << result.value("summary", "") << std::endl;
            if (result.contains("modelId") && !result["modelId"].get<std::string>().empty()) {
                std::cout << "模型已加入模型库: " << result["modelId"].get<std::string>() << std::endl;
            }
            if (result.contains("ggufPath")) std::cout << "GGUF: " << result["ggufPath"].get<std::string>() << std::endl;
            exitCode = 0;
            break;
        }
        if (status == "failed" || status == "cancelled") {
            std::cerr << "任务" << (status == "failed" ? "失败" : "已取消") << ": "
                      << task.value("errorMessage", "") << std::endl;
            exitCode = 2;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    downloads_->stop();
    return exitCode;
}

int App::runDownloadList(bool jsonOutput) {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "初始化失败: " << error << std::endl;
        return 1;
    }
    const json data = downloads_->list(100);
    if (jsonOutput) {
        std::cout << data.dump(2) << std::endl;
        return 0;
    }
    const auto& items = data["items"];
    if (items.empty()) {
        std::cout << "没有下载任务" << std::endl;
        return 0;
    }
    for (const auto& task : items) {
        std::cout << task.value("id", "") << "  " << task.value("statusLabel", task.value("status", ""))
                  << "  " << task.value("progress", 0) << "%  "
                  << humanBytes(task.value("downloadedBytes", 0ull)) << "/" << humanBytes(task.value("totalBytes", 0ull))
                  << "  " << task.value("repoId", "") << "@" << task.value("revision", "")
                  << "  → " << task.value("destination", "") << std::endl;
        if (!task.value("errorMessage", "").empty()) std::cout << "    错误: " << task.value("errorMessage", "") << std::endl;
    }
    return 0;
}

int App::runDownloadAction(const std::string& action, const std::string& id) {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "初始化失败: " << error << std::endl;
        return 1;
    }
    bool okAction = false;
    if (action == "pause") okAction = downloads_->pause(id, error);
    else if (action == "resume") okAction = downloads_->resume(id, error);
    else if (action == "cancel") okAction = downloads_->cancel(id, error);
    else if (action == "retry") okAction = downloads_->retry(id, error);
    else if (action == "delete") okAction = downloads_->remove(id, error);
    else {
        std::cerr << "未知下载操作: " << action << std::endl;
        return 1;
    }
    if (!okAction) {
        std::cerr << "操作失败: " << error << std::endl;
        return 2;
    }
    const json task = downloads_->getTask(id, false);
    std::cout << task.value("id", id) << "  " << task.value("statusLabel", task.value("status", ""))
              << "  " << task.value("progress", 0) << "%" << std::endl;
    return 0;
}

int App::runLlama(const std::string& action, const std::string& tag, const std::string& asset, bool jsonOutput) {
    std::string error;
    if (!initialize(error)) {
        std::cerr << "初始化失败: " << error << std::endl;
        return 1;
    }
    if (action == "status") {
        const json runtime = llama_->status();
        if (jsonOutput) {
            std::cout << json{{"runtime", runtime}, {"probe", llama_->probe()}}.dump(2) << std::endl;
            return 0;
        }
        std::cout << "安装目录: " << runtime.value("root", "") << "\n"
                  << "版本: " << runtime.value("versionLabel", "") << "\n"
                  << "后端: " << runtime.value("backendLabel", "") << "\n"
                  << "来源: " << runtime.value("detectionSource", "") << "\n"
                  << "资源: " << runtime.value("asset", "") << "\n"
                  << "量化工具: " << runtime["tools"].value("quantize", "") << "\n"
                  << "转换脚本: " << runtime.value("convertScript", "") << "\n";
        for (const auto& issue : runtime.value("issues", json::array())) {
            std::cout << "[问题] " << issue.get<std::string>() << "\n";
        }
        return runtime.value("installed", false) ? 0 : 3;
    }
    if (action == "check") {
        const LlamaCheckResult result = llama_->checkUpdate(true);
        if (!result.ok) {
            std::cerr << "检查失败 [" << result.code << "]: " << result.error << "\n";
            if (!result.suggestion.empty()) std::cerr << "建议: " << result.suggestion << "\n";
            return 2;
        }
        if (jsonOutput) {
            std::cout << result.data.dump(2) << std::endl;
        } else {
            std::cout << "状态: " << result.data.value("status", "") << "\n"
                      << "说明: " << result.data.value("message", "") << "\n"
                      << "本地: " << result.data["local"].value("versionLabel", "未安装") << "\n"
                      << "官方: " << result.data.value("remoteTag", "") << "\n"
                      << "资源: " << result.data["asset"].value("name", "") << " ("
                      << result.data["asset"].value("sizeLabel", "") << ")\n";
            const std::string fallback = result.data.value("fallbackReason", "");
            if (!fallback.empty()) std::cout << "提示: " << fallback << "\n";
        }
        const std::string status = result.data.value("status", "");
        if (status == "up_to_date" || status == "local_newer") return 0;
        return 3;
    }
    if (action == "assets") {
        const LlamaCheckResult result = llama_->releaseAssets(tag);
        if (!result.ok) {
            std::cerr << "获取资源失败 [" << result.code << "]: " << result.error << "\n";
            return 2;
        }
        if (jsonOutput) {
            std::cout << result.data.dump(2) << std::endl;
            return 0;
        }
        for (const auto& item : result.data.value("assets", json::array())) {
            std::cout << (item.value("compatible", false) ? "* " : "  ") << item.value("name", "")
                      << "  " << item.value("sizeLabel", "") << "  " << item.value("backendLabel", "") << "\n";
        }
        return 0;
    }
    if (action == "install") {
        json body{{"tag", tag}, {"asset", asset}, {"includeSource", true}};
        if (!llama_->install(body, error)) {
            std::cerr << "无法开始安装: " << error << std::endl;
            return 2;
        }
        int64_t lastPrint = 0;
        while (true) {
            const json job = llama_->jobStatus();
            const std::string status = job.value("status", "");
            const int64_t now = nowMs();
            if (now - lastPrint > 800) {
                lastPrint = now;
                std::cout << "[" << job.value("statusLabel", status) << "] " << job.value("progress", 0) << "%  "
                          << job.value("downloadedLabel", "") << "/" << job.value("totalLabel", "")
                          << "  " << job.value("message", "") << std::endl;
            }
            if (status == "completed") {
                std::cout << job.value("message", "") << "\n"
                          << "安装目录: " << llama_->status().value("releaseDir", "") << std::endl;
                return 0;
            }
            if (status == "failed") {
                std::cerr << "安装失败 [" << job.value("errorCode", "") << "]: " << job.value("errorMessage", "")
                          << "\n建议: " << job.value("errorSuggestion", "") << std::endl;
                return 2;
            }
            if (status == "cancelled") {
                std::cerr << "安装已取消" << std::endl;
                return 2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }
    std::cerr << "未知 llama 操作: " << action << std::endl;
    return 1;
}

void App::stop() {
    if (stopped_) return;
    stopped_ = true;
    if (watch_) watch_->stop();
    if (quality_) quality_->stop();
    if (downloads_) downloads_->stop();
    if (tasks_) tasks_->stop();
    if (ws_) ws_->stop();
    if (api_) api_->stop();
}

} // namespace mf
