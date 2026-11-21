#include "modelforge/task_manager.hpp"

#include "modelforge/crypto.hpp"
#include "modelforge/gguf_reader.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/model_scanner.hpp"
#include "modelforge/process_runner.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mf {

namespace {
std::string levelForLine(const std::string& line, bool isError) {
    const std::string lower = toLower(line);
    if (lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos ||
        lower.find("fatal") != std::string::npos || lower.find("traceback") != std::string::npos) return "ERROR";
    if (lower.find("warn") != std::string::npos || isError) return "WARNING";
    if (lower.find("debug") != std::string::npos || lower.find("verbose") != std::string::npos) return "DEBUG";
    return "INFO";
}

// Keeps the peak counters of one process invocation and persists them per stage,
// so a finished task can report how much CPU, memory and IO the child process
// actually used instead of host-wide numbers shared with every other program.
class ResourceTracker {
public:
    ResourceTracker(Database& db, std::string taskId) : db_(db), taskId_(std::move(taskId)) {}

    void begin(const std::string& stage) {
        stage_ = stage;
        startedAt_ = nowMs();
        persistedAt_ = 0;
        samples_ = 0;
        peakCpuMs_ = 0;
        peakMemory_ = 0;
        peakRead_ = 0;
        peakWrite_ = 0;
    }

    void sample(const ProcessSample& sample) {
        if (stage_.empty()) return;
        ++samples_;
        peakCpuMs_ = std::max(peakCpuMs_, sample.cpuTimeMs);
        peakMemory_ = std::max(peakMemory_, sample.peakMemoryBytes ? sample.peakMemoryBytes : sample.workingSetBytes);
        peakRead_ = std::max(peakRead_, sample.readBytes);
        peakWrite_ = std::max(peakWrite_, sample.writeBytes);
        const int64_t now = nowMs();
        if (persistedAt_ == 0 || now - persistedAt_ >= 2000) {
            persistedAt_ = now;
            persist(now - startedAt_);
        }
    }

    void commit() {
        if (stage_.empty()) return;
        persist(nowMs() - startedAt_);
        stage_.clear();
    }

private:
    void persist(int64_t durationMs) {
        if (durationMs < 0) durationMs = 0;
        db_.query("INSERT OR REPLACE INTO task_resources"
                  "(task_id,stage,cpu_ms,peak_memory,read_bytes,write_bytes,duration_ms,samples,updated_at) "
                  "VALUES(?,?,?,?,?,?,?,?,?)",
                  {taskId_, stage_, peakCpuMs_, static_cast<int64_t>(peakMemory_),
                   static_cast<int64_t>(peakRead_), static_cast<int64_t>(peakWrite_),
                   durationMs, samples_, isoNow()});
    }

    Database& db_;
    std::string taskId_;
    std::string stage_;
    int64_t startedAt_ = 0;
    int64_t persistedAt_ = 0;
    int64_t peakCpuMs_ = 0;
    uint64_t peakMemory_ = 0;
    uint64_t peakRead_ = 0;
    uint64_t peakWrite_ = 0;
    int samples_ = 0;
};

bool isTerminalStatus(const std::string& status) {
    return status == "SUCCEEDED" || status == "FAILED" || status == "CANCELED";
}

std::string stageLabel(const std::string& stage) {
    if (stage == "QUEUED") return "等待中";
    if (stage == "SCANNING") return "扫描模型";
    if (stage == "CHECKING") return "检查模型";
    if (stage == "LOADING_CONFIG") return "加载配置";
    if (stage == "CONVERTING") return "权重转换与 GGUF 写入";
    if (stage == "QUANTIZING") return "量化处理";
    if (stage == "VERIFYING") return "文件验证";
    if (stage == "COMPLETED") return "转换完成";
    if (stage == "FAILED") return "转换失败";
    if (stage == "CANCELED") return "已取消";
    if (stage == "PAUSED") return "已暂停";
    return stage;
}
} // namespace

TaskManager::TaskManager(Database& db, SettingsService& settings, ConverterService& converter, EventHub& events)
    : db_(db), settings_(settings), converter_(converter), events_(events) {
    ensureDirs();
}

TaskManager::~TaskManager() { stop(); }

void TaskManager::ensureDirs() {
    std::string error;
    ensureDirectory(settings_.logsDir(), error);
    ensureDirectory(settings_.dataDir() / "reports", error);
    ensureDirectory(settings_.tempDir(), error);
}

bool TaskManager::tryAcquireSchedulerLock() {
    if (schedulerOwned_.load()) return true;
    std::error_code ec;
    fs::create_directories(settings_.dataDir(), ec);
    const fs::path lockPath = settings_.dataDir() / "scheduler.lock";
#ifdef _WIN32
    HANDLE handle = CreateFileW(lockPath.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    schedulerLockHandle_ = handle;
#else
    const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    schedulerLockFd_ = fd;
#endif
    schedulerOwned_.store(true);
    Logger::instance().info("Task scheduler lock acquired: " + pathToUtf8(lockPath));
    return true;
}

void TaskManager::releaseSchedulerLock() {
    if (!schedulerOwned_.exchange(false)) return;
#ifdef _WIN32
    if (schedulerLockHandle_) {
        CloseHandle(static_cast<HANDLE>(schedulerLockHandle_));
        schedulerLockHandle_ = nullptr;
    }
#else
    if (schedulerLockFd_ >= 0) {
        flock(schedulerLockFd_, LOCK_UN);
        ::close(schedulerLockFd_);
        schedulerLockFd_ = -1;
    }
#endif
}

void TaskManager::start() {
    stop_.store(false);
    scheduler_ = std::thread([this] { schedulerLoop(); });
}

void TaskManager::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_.load()) return;
        stop_.store(true);
        for (auto& pair : runtime_) {
            if (pair.second && !isTerminalStatus(pair.second->task.status)) pair.second->cancel.store(true);
        }
    }
    cv_.notify_all();
    if (scheduler_.joinable()) scheduler_.join();
    releaseSchedulerLock();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : workers_) if (pair.second.joinable()) threads.push_back(std::move(pair.second));
        workers_.clear();
    }
    for (auto& thread : threads) if (thread.joinable()) thread.join();
}

void TaskManager::schedulerLoop() {
    while (true) {
        std::vector<std::shared_ptr<Runtime>> toStart;
        {
            if (stop_.load()) break;
        if (!schedulerOwned_.load() && !tryAcquireSchedulerLock()) {
            std::unique_lock<std::mutex> waitLock(mutex_);
            cv_.wait_for(waitLock, std::chrono::milliseconds(1500), [&] { return stop_.load(); });
            continue;
        }
        if (!reconciled_) {
            std::string dbError;
            db_.query("UPDATE conversion_tasks SET status='FAILED', stage='FAILED', error_code='PROCESS_RESTARTED', "
                      "error_message='ModelForge 上次退出时任务仍在运行，已标记为中断', finished_at=? "
                      "WHERE status='RUNNING'", {nowMs()}, &dbError);
            reconciled_ = true;
        }
        std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(800), [&] { return stop_.load(); });
            if (stop_.load()) break;
            const int maxConcurrent = std::max(1, settings_.get().maxConcurrent);
            if (running_ >= maxConcurrent) continue;
            const int available = maxConcurrent - running_;
            const auto rows = db_.query(
                "SELECT * FROM conversion_tasks WHERE status='QUEUED' ORDER BY priority DESC, created_at ASC LIMIT ?",
                {available});
            for (const auto& row : rows) {
                const std::string id = row.value("id", "");
                auto it = runtime_.find(id);
                if (it == runtime_.end()) {
                    auto rt = std::make_shared<Runtime>();
                    rt->task = taskFromRow(row);
                    runtime_[id] = rt;
                    it = runtime_.find(id);
                }
                if (it->second->cancel.load()) continue;
                it->second->task.status = "RUNNING";
                it->second->task.startedAt = nowMs();
                it->second->task.stage = "SCANNING";
                db_.query("UPDATE conversion_tasks SET status='RUNNING',started_at=?,stage='SCANNING' WHERE id=? AND status='QUEUED'",
                          {it->second->task.startedAt, id});
                running_++;
                toStart.push_back(it->second);
            }
        }
        for (auto& rt : toStart) {
            const std::string id = rt->task.id;
            std::lock_guard<std::mutex> lock(mutex_);
            workers_[id] = std::thread([this, id] { runTask(id); });
        }
    }
}

std::shared_ptr<TaskManager::Runtime> TaskManager::findRuntime(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runtime_.find(id);
    return it == runtime_.end() ? nullptr : it->second;
}

TaskRecord TaskManager::taskFromRow(const json& row) const {
    TaskRecord task;
    task.id = row.value("id", "");
    task.modelId = row.value("model_id", "");
    task.modelName = row.value("model_name", "");
    task.status = row.value("status", "QUEUED");
    task.stage = row.value("stage", "QUEUED");
    task.progress = row.value("progress", 0);
    task.progressMode = row.value("progress_mode", "estimated");
    task.priority = row.value("priority", 0);
    task.inputPath = row.value("input_path", "");
    task.outputPath = row.value("output_path", "");
    task.outputSize = row.value("output_size", 0ull);
    task.createdAt = row.value("created_at", 0ll);
    task.startedAt = row.value("started_at", 0ll);
    task.finishedAt = row.value("finished_at", 0ll);
    task.elapsedMs = row.value("elapsed_ms", 0ll);
    task.errorCode = row.value("error_code", "");
    task.errorMessage = row.value("error_message", "");
    task.sha256 = row.value("sha256", "");
    task.logsPath = row.value("logs_path", "");
    task.verificationJson = row.value("verification_json", "");
    try { task.params = ConversionParams::fromJson(json::parse(row.value("params_json", "{}"))); }
    catch (...) {}
    return task;
}

void TaskManager::persistTask(const TaskRecord& task) {
    db_.query("UPDATE conversion_tasks SET status=?,stage=?,progress=?,progress_mode=?,priority=?,output_path=?,output_size=?,"
              "error_code=?,error_message=?,sha256=?,verification_json=?,started_at=?,finished_at=?,elapsed_ms=? WHERE id=?",
              {task.status, task.stage, task.progress, task.progressMode, task.priority, task.outputPath,
               static_cast<int64_t>(task.outputSize), task.errorCode, task.errorMessage, task.sha256,
               task.verificationJson, task.startedAt, task.finishedAt, task.elapsedMs, task.id}, nullptr);
}

bool TaskManager::createTask(ConversionParams params, std::string& taskId, std::string& code, std::string& error) {
    ensureDirs();
    if (!converter_.validateParams(params, code, error)) return false;
    ModelScanResult scan;
    try {
        scan = ModelScanner::scan(pathFromUtf8(params.inputPath));
    } catch (const std::exception& ex) {
        code = "SCAN_FAILED";
        error = ex.what();
        return false;
    }
    if (!scan.complete) {
        code = "MODEL_INCOMPLETE";
        error = scan.errors.empty() ? "模型文件不完整" : join(scan.errors, "；");
        return false;
    }
    if (scan.config.contains("pad_token_id") && scan.config["pad_token_id"].is_number_integer() &&
        scan.config["pad_token_id"].get<int>() < 0) {
        code = "CONFIG_SPECIAL_TOKEN_INVALID";
        error = "config.json 的 pad_token_id 为负数，当前 llama.cpp 转换器无法写入该特殊 Token。请在模型配置中设为有效 Token ID 或移除该字段后重新导入。";
        return false;
    }
    const json compatibility = converter_.compatibility(scan);
    if (!compatibility.value("supported", false)) {
        code = compatibility.value("code", std::string("ARCH_UNSUPPORTED"));
        error = compatibility.value("reason", "当前转换工具不支持该架构");
        return false;
    }
    if (params.modelId.empty()) params.modelId = ensureModelRecord(scan);
    if (params.modelName.empty()) params.modelName = scan.name;
    if (params.outputName.empty()) params.outputName = sanitizeFileName(scan.name);
    if (params.quantType.empty()) params.quantType = "";

    taskId = newId("task");
    TaskRecord task;
    task.id = taskId;
    task.modelId = params.modelId;
    task.modelName = params.modelName;
    task.status = "QUEUED";
    task.stage = "QUEUED";
    task.progress = 0;
    task.progressMode = "stage";
    task.priority = params.priority;
    task.inputPath = params.inputPath;
    task.createdAt = nowMs();
    task.logsPath = pathToUtf8(logFilePath(taskId));
    task.params = params;

    std::string dbError;
    db_.query("INSERT INTO conversion_tasks(id,model_id,model_name,status,stage,progress,progress_mode,priority,input_path,"
              "params_json,logs_path,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
              {task.id, task.modelId, task.modelName, task.status, task.stage, task.progress, task.progressMode,
               task.priority, task.inputPath, params.toJson().dump(), task.logsPath, task.createdAt}, &dbError);
    if (!dbError.empty()) { code = "DB_ERROR"; error = dbError; return false; }
    db_.query("INSERT INTO conversion_history(id,model_id,task_id,event,detail,created_at) VALUES(?,?,?,?,?,?)",
              {newId("hist"), task.modelId, task.id, "TASK_CREATED", "创建转换任务", isoNow()});

    auto rt = std::make_shared<Runtime>();
    rt->task = task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtime_[task.id] = rt;
    }
    const std::string logPath = task.logsPath;
    {
        std::ofstream log(pathFromUtf8(logPath).u8string(), std::ios::app | std::ios::binary);
        // File is created eagerly so download links are valid while queued.
    }
    appendLog(rt, "INFO", "任务已创建，等待调度。输出目录: " + params.outputDir);
    events_.publish(json{{"type", "task.created"}, {"taskId", task.id}, {"task", task.toJson()}});
    cv_.notify_all();
    return true;
}

void TaskManager::updateStatus(const std::shared_ptr<Runtime>& rt, const std::string& status, const std::string& stage,
                               const std::string& detail, const std::string& code, const std::string& message) {
    TaskRecord copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rt->task.status = status;
        rt->task.stage = stage;
        rt->task.errorCode = code;
        rt->task.errorMessage = message;
        rt->stageDetail = detail;
        if (!isTerminalStatus(status) && stage != "QUEUED" && stage != "PAUSED") {
            rt->task.elapsedMs = nowMs() - (rt->task.startedAt > 0 ? rt->task.startedAt : rt->task.createdAt);
        }
        copy = rt->task;
    }
    persistTask(copy);
    events_.publish(json{{"type", "task.status"}, {"taskId", copy.id}, {"task", copy.toJson()},
                         {"status", status}, {"stage", stage}, {"detail", detail}});
    if (!detail.empty()) appendLog(rt, "INFO", detail);
    cv_.notify_all();
}

void TaskManager::updateProgress(const std::shared_ptr<Runtime>& rt, int progress, const std::string& mode,
                                 const std::string& detail, uint64_t processed, uint64_t total) {
    TaskRecord copy;
    bool shouldPersist = false;
    const int64_t now = nowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rt->task.progress = progress;
        rt->task.progressMode = mode;
        rt->stageDetail = detail;
        rt->processedBytes = processed;
        rt->totalBytes = total;
        if (now - rt->lastPersistAt >= 500) {
            rt->lastPersistAt = now;
            shouldPersist = true;
        }
        copy = rt->task;
    }
    if (shouldPersist) persistTask(copy);
    json event{{"type", "task.progress"}, {"taskId", copy.id}, {"task", copy.toJson()}, {"progress", progress},
               {"mode", mode}, {"detail", detail}, {"processedBytes", processed}, {"totalBytes", total},
               {"elapsedMs", copy.elapsedMs}};
    events_.publish(event);
}

void TaskManager::appendLog(const std::shared_ptr<Runtime>& rt, const std::string& level, const std::string& message) {
    if (!rt || message.empty()) return;
    std::string clean = sanitizeSingleLine(message, 16000);
    std::lock_guard<std::mutex> lock(logMutex_);
    const int64_t seq = ++rt->logSeq;
    json entry{{"seq", seq}, {"time", isoNow()}, {"level", level}, {"message", clean}};
    std::error_code ec;
    std::ofstream out(pathFromUtf8(rt->task.logsPath).u8string(), std::ios::app | std::ios::binary);
    if (out) out << entry.dump() << "\n";
    events_.publish(json{{"type", "task.log"}, {"taskId", rt->task.id}, {"log", entry}});
    if (level == "ERROR" || level == "WARNING") {
        db_.query("INSERT INTO task_logs(task_id,seq,level,message,created_at) VALUES(?,?,?,?,?)",
                  {rt->task.id, seq, level, clean, isoNow()});
    }
}

std::string TaskManager::classifyError(const std::string& text, int exitCode, const std::string& stage) const {
    const std::string lower = toLower(text);
    if (lower.find("out of memory") != std::string::npos || lower.find("memoryerror") != std::string::npos ||
        lower.find("bad_alloc") != std::string::npos || lower.find("std::bad_alloc") != std::string::npos) return "OUT_OF_MEMORY";
    if (lower.find("no space left") != std::string::npos || lower.find("disk full") != std::string::npos ||
        lower.find("not enough space") != std::string::npos) return "DISK_FULL";
    if (lower.find("no such file") != std::string::npos || lower.find("file not found") != std::string::npos) return "MODEL_FILE_MISSING";
    if (lower.find("modulenotfounderror") != std::string::npos || lower.find("importerror") != std::string::npos) return "PYTHON_ENV_INVALID";
    if (lower.find("unsupported architecture") != std::string::npos || lower.find("model architecture") != std::string::npos ||
        lower.find("not supported") != std::string::npos) return "ARCH_UNSUPPORTED";
    if (lower.find("permission denied") != std::string::npos || lower.find("access is denied") != std::string::npos) return "OUTPUT_DIR_UNWRITABLE";
    if (lower.find("invalid magic") != std::string::npos || lower.find("gguf") != std::string::npos) return "GGUF_VERIFY_FAILED";
    if (stage == "QUANTIZING") return "QUANTIZATION_FAILED";
    if (exitCode != 0) return "PROCESS_EXITED";
    return "UNKNOWN_ERROR";
}

std::string TaskManager::errorSuggestion(const std::string& code) const {
    if (code == "OUT_OF_MEMORY") return "关闭其他占用内存的程序，或改用更小的模型/更低精度量化；也可尝试 --use-temp-file。";
    if (code == "DISK_FULL") return "释放输出磁盘空间，或更换输出目录；量化需要同时保留中间 GGUF 与最终文件。";
    if (code == "MODEL_FILE_MISSING") return "检查模型目录和分片索引，重新导入完整模型文件。";
    if (code == "PYTHON_ENV_INVALID") return "在转换环境执行 pip install -r llama.cpp/requirements/requirements-convert_hf_to_gguf.txt，并在设置中修正 Python 路径。";
    if (code == "ARCH_UNSUPPORTED") return "升级 llama.cpp 转换脚本，或换用脚本已注册的模型架构。";
    if (code == "ARCH_UNSUPPORTED_CUSTOM") {
        return "该架构需要专用转换器与推理引擎，不能通过升级 llama.cpp 或强制转换解决。"
               "语音/多模态模型请使用其官方推理代码或对应的 GGUF 引擎；ModelForge 不会生成可能损坏的 GGUF。";
    }
    if (code == "QUANTIZATION_FAILED") return "确认量化类型与模型匹配，检查 llama-quantize 版本；尝试从更高精度中间 GGUF 重新量化。";
    if (code == "CONVERTER_MISSING" || code == "QUANTIZER_MISSING") return "在设置页配置有效的 llama.cpp 工具路径。";
    if (code == "GGUF_VERIFY_FAILED") return "保留中间文件，检查磁盘完整性与工具退出日志；不要使用损坏的 GGUF。";
    if (code == "OUTPUT_EXISTS") return "启用覆盖，或修改输出名称。";
    if (code == "CONFIG_SPECIAL_TOKEN_INVALID") return "将 config.json 的 pad_token_id 改为有效 Token ID（例如 0）或移除该字段；修改前备份模型目录。";
    return "查看完整日志末尾的错误行，检查工具路径、模型完整性与磁盘空间后重试。";
}

uint64_t TaskManager::estimatedOutputSize(const ModelScanResult& scan, const std::string& type) {
    json estimated = converter_.estimates(scan);
    const std::string target = type;
    for (const auto& option : estimated["options"]) {
        if (toLower(option.value("type", "")) == toLower(target)) return option.value("estimatedSize", 0ull);
    }
    return 0;
}

void TaskManager::runTask(const std::string& id) {
    auto rt = findRuntime(id);
    if (!rt) {
        std::lock_guard<std::mutex> lock(mutex_);
        --running_;
        cv_.notify_all();
        return;
    }
    auto release = [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        --running_;
        cv_.notify_all();
    };
    ResourceTracker resourceTracker(db_, id);
    try {
        updateStatus(rt, "RUNNING", "SCANNING", "正在扫描模型文件");
        updateProgress(rt, -1, "indeterminate", "读取 config.json 与 Safetensors 头部", 0, 0);
        const ModelScanResult scan = ModelScanner::scan(pathFromUtf8(rt->task.inputPath));
        if (!scan.complete) {
            const std::string detail = scan.errors.empty() ? "模型不完整" : join(scan.errors, "；");
            finishTask(rt, "FAILED", "FAILED", "MODEL_INCOMPLETE", detail);
            release();
            return;
        }
        appendLog(rt, "INFO", "模型扫描完成: " + scan.name + " · " + scan.architectureFamily +
                  " · " + humanBytes(scan.totalSize) + " · " + std::to_string(scan.shardCount) + " 个分片");
        if (rt->cancel.load()) {
            finishTask(rt, "CANCELED", "CANCELED", "CANCELLED", "用户取消了任务");
            release();
            return;
        }

        updateStatus(rt, "RUNNING", "CHECKING", "检查架构兼容性、输出目录与磁盘空间");
        const json compatibility = converter_.compatibility(scan);
        if (!compatibility.value("supported", false)) {
            finishTask(rt, "FAILED", "FAILED", "ARCH_UNSUPPORTED", compatibility.value("reason", "模型架构不受支持"));
            release();
            return;
        }
        if (compatibility.value("match", "") == "heuristic") {
            appendLog(rt, "WARNING", "架构匹配为启发式结果，请确认模型可被当前 llama.cpp 版本转换。");
        }
        const std::string intermediateType = rt->task.params.outType;
        const fs::path intermediate = converter_.intermediatePath(rt->task.params);
        const fs::path final = converter_.finalPath(rt->task.params);
        const uint64_t intermediateEstimate = estimatedOutputSize(scan, toLower(intermediateType));
        const uint64_t finalEstimate = rt->task.params.quantType.empty()
            ? intermediateEstimate : estimatedOutputSize(scan, rt->task.params.quantType);
        const uint64_t diskRequired = intermediateEstimate + (rt->task.params.quantType.empty() ? 0 : finalEstimate) + 64ull * MB;
        const uint64_t diskFree = diskFreeBytes(final.parent_path());
        if (diskFree > 0 && diskFree < diskRequired) {
            finishTask(rt, "FAILED", "FAILED", "DISK_FULL",
                       "磁盘空间不足：预计需要 " + humanBytes(diskRequired) + "，当前可用 " + humanBytes(diskFree));
            release();
            return;
        }
        if (availableSystemMemory() > 0 && availableSystemMemory() < 512ull * MB) {
            appendLog(rt, "WARNING", "当前可用系统内存低于 512 MiB，转换可能因内存不足失败。");
        }
        appendLog(rt, "INFO", "输出目录: " + pathToUtf8(final.parent_path()));
        appendLog(rt, "INFO", "预计量化输出体积: " + humanBytes(finalEstimate) + "（估算值，实际以文件为准）");

        updateStatus(rt, "RUNNING", "LOADING_CONFIG", "加载 config.json，准备转换参数");
        json preview = converter_.commandPreview(rt->task.params);
        appendLog(rt, "DEBUG", "转换命令: " + preview.value("convert", ""));
        if (!rt->task.params.quantType.empty()) appendLog(rt, "DEBUG", "量化命令: " + preview.value("quantize", ""));

        fs::path backupPath;
        std::error_code ec;
        auto backupExisting = [&](const fs::path& target) {
            if (fs::exists(target, ec) && rt->task.params.overwrite) {
                backupPath = target;
                backupPath += ".modelforge-backup-" + std::to_string(nowMs());
                fs::rename(target, backupPath, ec);
                if (ec) throw std::runtime_error("无法备份已存在的输出文件: " + ec.message());
                appendLog(rt, "INFO", "已存在输出文件，创建安全备份: " + pathToUtf8(backupPath));
            }
        };
        auto restoreBackup = [&]() {
            if (!backupPath.empty() && fs::exists(backupPath, ec)) {
                fs::remove(final, ec);
                ec.clear();
                fs::rename(backupPath, final, ec);
                appendLog(rt, "INFO", ec ? "恢复原输出文件失败: " + ec.message() : "已恢复原有输出文件。");
            }
        };

        const fs::path convertTarget = rt->task.params.quantType.empty() ? final : intermediate;
        backupExisting(convertTarget);
        updateStatus(rt, "RUNNING", "CONVERTING", "调用 convert_hf_to_gguf.py 转换权重");
        const AppSettings s = settings_.get();
        std::vector<std::string> convertArgs = {s.convertScript, "--outtype", rt->task.params.outType,
                                                "--outfile", pathToUtf8(convertTarget)};
        if (rt->task.params.useTempFile) convertArgs.push_back("--use-temp-file");
        if (rt->task.params.noLazy) convertArgs.push_back("--no-lazy");
        if (!rt->task.params.modelName.empty()) {
            convertArgs.push_back("--model-name");
            convertArgs.push_back(sanitizeSingleLine(rt->task.params.modelName, 200));
        }
        convertArgs.push_back(rt->task.inputPath);

        auto fileTick = [&](const fs::path& file, uint64_t expected, int base, int span, const std::string& label) {
            std::error_code sizeEc;
            const uint64_t current = fs::exists(file, sizeEc) ? static_cast<uint64_t>(fs::file_size(file, sizeEc)) : 0;
            if (current > 0 && expected > 0) {
                const double ratio = std::min(1.0, static_cast<double>(current) / static_cast<double>(expected));
                const int progress = std::min(base + span, base + static_cast<int>(ratio * span));
                updateProgress(rt, progress, "estimated", label + "（按输出文件增长估算）", current, expected);
            } else {
                updateProgress(rt, -1, "indeterminate", label, current, expected);
            }
        };

        ProcessOptions convert;
        convert.executable = s.pythonPath;
        convert.args = convertArgs;
        convert.cancelFlag = &rt->cancel;
        convert.workingDirectory = pathFromUtf8(rt->task.inputPath);
        convert.onLine = [&](const std::string& line, bool isError) {
            appendLog(rt, levelForLine(line, isError), line);
        };
        const int convertEnd = rt->task.params.quantType.empty() ? 82 : 55;
        convert.onTick = [&] { fileTick(convertTarget, intermediateEstimate, 12, convertEnd - 12, "权重转换中"); };
        convert.onSample = [&](const ProcessSample& sample) { resourceTracker.sample(sample); };
        resourceTracker.begin("CONVERTING");
        const ProcessResult convertResult = ProcessRunner::run(convert);
        resourceTracker.commit();
        if (convertResult.canceled || rt->cancel.load()) {
            if (fs::exists(convertTarget, ec) && rt->task.params.quantType.empty() && backupPath.empty()) fs::remove(convertTarget, ec);
            restoreBackup();
            finishTask(rt, "CANCELED", "CANCELED", "CANCELLED", "用户取消了转换任务");
            release();
            return;
        }
        if (!convertResult.started || convertResult.exitCode != 0) {
            restoreBackup();
            const std::string code = convertResult.started
                ? classifyError(convertResult.error, convertResult.exitCode, "CONVERTING")
                : "PROCESS_START_FAILED";
            finishTask(rt, "FAILED", "FAILED", code,
                       "权重转换失败，退出码 " + std::to_string(convertResult.exitCode) +
                       (convertResult.error.empty() ? "" : "：" + convertResult.error));
            release();
            return;
        }
        if (!fs::exists(convertTarget, ec)) {
            restoreBackup();
            finishTask(rt, "FAILED", "FAILED", "OUTPUT_MISSING", "转换进程结束，但未找到中间 GGUF 文件");
            release();
            return;
        }
        GgufInfo intermediateInfo = GgufReader::read(convertTarget, false);
        if (!intermediateInfo.valid) {
            restoreBackup();
            finishTask(rt, "FAILED", "FAILED", "GGUF_VERIFY_FAILED",
                       "中间 GGUF 结构无效: " + join(intermediateInfo.errors, "；"));
            release();
            return;
        }
        updateProgress(rt, convertEnd, "measured", "中间 GGUF 已写入并通过结构检查", intermediateInfo.fileSize, intermediateInfo.fileSize);
        appendLog(rt, "INFO", "中间 GGUF 完成: " + pathToUtf8(convertTarget) + " · " + humanBytes(intermediateInfo.fileSize));

        if (!rt->task.params.quantType.empty()) {
            if (rt->cancel.load()) {
                restoreBackup();
                finishTask(rt, "CANCELED", "CANCELED", "CANCELLED", "用户取消了任务");
                release();
                return;
            }
            backupExisting(final);
            updateStatus(rt, "RUNNING", "QUANTIZING", "调用 llama-quantize 执行 " + rt->task.params.quantType);
            appendLog(rt, "INFO", "开始量化: " + rt->task.params.quantType + "，线程数 " +
                      std::to_string(rt->task.params.threads));
            ProcessOptions quantize;
            quantize.executable = s.quantizePath;
            quantize.args = {pathToUtf8(intermediate), pathToUtf8(final), rt->task.params.quantType,
                             std::to_string(rt->task.params.threads)};
            quantize.cancelFlag = &rt->cancel;
            quantize.workingDirectory = pathFromUtf8(rt->task.params.outputDir);
            quantize.onLine = [&](const std::string& line, bool isError) { appendLog(rt, levelForLine(line, isError), line); };
            quantize.onTick = [&] { fileTick(final, finalEstimate, 58, 34, "量化输出中"); };
            quantize.onSample = [&](const ProcessSample& sample) { resourceTracker.sample(sample); };
            resourceTracker.begin("QUANTIZING");
            const ProcessResult quantResult = ProcessRunner::run(quantize);
            resourceTracker.commit();
            if (quantResult.canceled || rt->cancel.load()) {
                if (fs::exists(final, ec)) fs::remove(final, ec);
                restoreBackup();
                finishTask(rt, "CANCELED", "CANCELED", "CANCELLED", "用户取消了量化任务");
                release();
                return;
            }
            if (!quantResult.started || quantResult.exitCode != 0) {
                if (fs::exists(final, ec)) fs::remove(final, ec);
                restoreBackup();
                finishTask(rt, "FAILED", "FAILED",
                           quantResult.started ? classifyError(quantResult.error, quantResult.exitCode, "QUANTIZING") : "PROCESS_START_FAILED",
                           "量化失败，退出码 " + std::to_string(quantResult.exitCode));
                release();
                return;
            }
            if (!fs::exists(final, ec)) {
                restoreBackup();
                finishTask(rt, "FAILED", "FAILED", "OUTPUT_MISSING", "量化进程结束，但未找到输出 GGUF");
                release();
                return;
            }
            if (!backupPath.empty()) {
                fs::remove(backupPath, ec);
                backupPath.clear();
            }
        }

        updateStatus(rt, "RUNNING", "VERIFYING", "验证 GGUF 结构与文件完整性");
        GgufInfo info = GgufReader::read(final, true);
        if (!info.valid) {
            finishTask(rt, "FAILED", "FAILED", "GGUF_VERIFY_FAILED",
                       "最终 GGUF 验证失败: " + join(info.errors, "；"));
            release();
            return;
        }
        json verification{{"structurallyValid", true}, {"ggufVersion", info.version},
                          {"tensorCount", info.tensorCount}, {"metadataCount", info.metadataCount},
                          {"architecture", info.architecture}, {"quantType", info.quantType},
                          {"fileSize", info.fileSize}, {"verifiedAt", isoNow()}};
        appendLog(rt, "INFO", "GGUF 结构验证通过: version=" + std::to_string(info.version) +
                  " tensors=" + std::to_string(info.tensorCount) + " arch=" + info.architecture);

        if (rt->task.params.deepVerify && !s.llamaCliPath.empty()) {
            appendLog(rt, "INFO", "执行 llama-cli 深度加载验证");
            ProcessOptions cli;
            cli.executable = s.llamaCliPath;
            cli.args = {"-m", pathToUtf8(final), "-p", "Hello", "-n", "1", "--no-warmup", "--simple-io", "-st"};
            cli.cancelFlag = &rt->cancel;
            cli.timeoutMs = 300000;
            int outputLines = 0;
            bool cliReportedError = false;
            cli.onLine = [&](const std::string& line, bool) {
                const std::string lower = toLower(line);
                if (lower.find("error:") != std::string::npos || lower.find("failed to") != std::string::npos) cliReportedError = true;
                if (outputLines++ < 30) appendLog(rt, "DEBUG", "[llama-cli] " + line);
            };
            const ProcessResult cliResult = ProcessRunner::run(cli);
            verification["deepValidation"] = json{{"ran", true}, {"exitCode", cliResult.exitCode},
                                                   {"passed", cliResult.started && cliResult.exitCode == 0 && !cliReportedError}};
            if (!cliResult.started || cliResult.exitCode != 0 || cliReportedError) {
                finishTask(rt, "FAILED", "FAILED", "DEEP_VERIFY_FAILED", "llama-cli 加载验证失败，退出码 " + std::to_string(cliResult.exitCode));
                release();
                return;
            }
        } else {
            verification["deepValidation"] = json{{"ran", false}, {"reason", "未启用深度验证或 llama-cli 不可用"}};
        }

        std::string digest;
        if (rt->task.params.computeSha256) {
            updateStatus(rt, "RUNNING", "VERIFYING", "计算输出文件 SHA256");
            digest = sha256File(final, [&](uint64_t done, uint64_t total) {
                const int pct = total ? 94 + static_cast<int>((static_cast<double>(done) / static_cast<double>(total)) * 5.0) : 95;
                updateProgress(rt, pct, "measured", "计算 SHA256", done, total);
            });
            verification["sha256"] = digest;
        }
        const uint64_t outputSize = static_cast<uint64_t>(fs::file_size(final, ec));
        verification["reportPath"] = pathToUtf8(settings_.dataDir() / "reports" / (rt->task.id + ".json"));
        json manifest{
            {"taskId", rt->task.id}, {"modelName", rt->task.modelName}, {"inputPath", rt->task.inputPath},
            {"outputPath", pathToUtf8(final)}, {"outputSize", outputSize}, {"quantType", rt->task.params.quantType},
            {"outType", rt->task.params.outType}, {"threads", rt->task.params.threads}, {"params", rt->task.params.toJson()},
            {"gguf", info.toJson()}, {"verification", verification}, {"sha256", digest},
            {"tool", json{{"convertScript", s.convertScript}, {"quantize", s.quantizePath}}},
            {"environment", json{{"os", getOsName()}, {"host", getHostName()}}},
            {"completedAt", isoNow()}
        };
        writeTextFileAtomic(pathFromUtf8(verification["reportPath"].get<std::string>()), manifest.dump(2));

        if (!rt->task.params.keepIntermediate && !rt->task.params.quantType.empty() && fs::exists(intermediate, ec)) {
            fs::remove(intermediate, ec);
            if (ec) appendLog(rt, "WARNING", "无法删除中间文件: " + ec.message());
            else appendLog(rt, "INFO", "已删除中间 GGUF（保留最终量化文件）");
        }
        if (!backupPath.empty()) {
            fs::remove(backupPath, ec);
            backupPath.clear();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            rt->task.outputPath = pathToUtf8(final);
            rt->task.outputSize = outputSize;
            rt->task.sha256 = digest;
            rt->task.verificationJson = verification.dump();
        }
        updateProgress(rt, 100, "measured", "转换完成并通过结构验证", outputSize, outputSize);
        finishTask(rt, "SUCCEEDED", "COMPLETED", "", "转换完成: " + pathToUtf8(final));

        db_.query("INSERT INTO model_versions(id,model_id,task_id,quant_type,path,size,sha256,tool_version,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
                  {newId("ver"), rt->task.modelId, rt->task.id,
                   rt->task.params.quantType.empty() ? toLower(rt->task.params.outType) : rt->task.params.quantType,
                   pathToUtf8(final), static_cast<int64_t>(outputSize), digest,
                   s.quantizePath, isoNow()});
        db_.query("INSERT INTO conversion_history(id,model_id,task_id,event,detail,created_at) VALUES(?,?,?,?,?,?)",
                  {newId("hist"), rt->task.modelId, rt->task.id, "TASK_SUCCEEDED", "转换成功: " + pathToUtf8(final), isoNow()});
    } catch (const std::exception& ex) {
        try {
            finishTask(rt, "FAILED", "FAILED", "INTERNAL_ERROR", std::string("任务异常: ") + ex.what());
        } catch (...) {}
    } catch (...) {
        try { finishTask(rt, "FAILED", "FAILED", "INTERNAL_ERROR", "任务发生未知异常"); } catch (...) {}
    }
    release();
}

void TaskManager::finishTask(const std::shared_ptr<Runtime>& rt, const std::string& status, const std::string& stage,
                             const std::string& code, const std::string& message) {
    TaskRecord copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rt->task.status = status;
        rt->task.stage = stage;
        rt->task.errorCode = code;
        rt->task.errorMessage = message;
        rt->task.finishedAt = nowMs();
        rt->task.elapsedMs = rt->task.finishedAt - (rt->task.startedAt > 0 ? rt->task.startedAt : rt->task.createdAt);
        if (status == "SUCCEEDED" && rt->task.progress < 0) rt->task.progress = 100;
        copy = rt->task;
    }
    persistTask(copy);
    events_.publish(json{{"type", "task.status"}, {"taskId", copy.id}, {"task", copy.toJson()},
                         {"status", status}, {"stage", stage}, {"detail", message}});
    if (status == "FAILED") {
        appendLog(rt, "ERROR", message);
        events_.publish(json{{"type", "task.error"}, {"taskId", copy.id}, {"code", code},
                             {"message", message}, {"suggestion", errorSuggestion(code)}});
        db_.query("INSERT INTO conversion_history(id,model_id,task_id,event,detail,created_at) VALUES(?,?,?,?,?,?)",
                  {newId("hist"), copy.modelId, copy.id, "TASK_FAILED", code + ": " + message, isoNow()});
    } else if (status == "CANCELED") {
        appendLog(rt, "WARNING", message);
        db_.query("INSERT INTO conversion_history(id,model_id,task_id,event,detail,created_at) VALUES(?,?,?,?,?,?)",
                  {newId("hist"), copy.modelId, copy.id, "TASK_CANCELED", message, isoNow()});
    }
    events_.publish(json{{"type", status == "SUCCEEDED" ? "task.completed" : (status == "FAILED" ? "task.failed" : "task.canceled")},
                         {"taskId", copy.id}, {"task", copy.toJson()}});
}

json TaskManager::listTasks(const std::string& status, const std::string& modelId, int limit) {
    std::string sql = "SELECT * FROM conversion_tasks WHERE 1=1";
    std::vector<json> params;
    if (!status.empty()) { sql += " AND status=?"; params.push_back(status); }
    if (!modelId.empty()) { sql += " AND model_id=?"; params.push_back(modelId); }
    sql += " ORDER BY created_at DESC LIMIT ?";
    params.push_back(std::max(1, std::min(limit, 500)));
    json result = json::array();
    for (const auto& row : db_.query(sql, params)) {
        TaskRecord task = taskFromRow(row);
        if (task.status == "RUNNING" && task.startedAt > 0) task.elapsedMs = nowMs() - task.startedAt;
        json item = task.toJson();
        item["stageLabel"] = stageLabel(task.stage);
        result.push_back(item);
    }
    return result;
}

json TaskManager::getTask(const std::string& id) {
    const auto rows = db_.query("SELECT * FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) return nullptr;
    TaskRecord task = taskFromRow(rows[0]);
    auto rt = findRuntime(id);
    if (rt && schedulerOwned_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        task = rt->task;
        if (task.status == "RUNNING" && task.startedAt > 0) task.elapsedMs = nowMs() - task.startedAt;
    }
    json result = task.toJson();
    result["stageLabel"] = stageLabel(task.stage);
    if (rt && schedulerOwned_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);
        result["stageDetail"] = rt->stageDetail;
        result["processedBytes"] = rt->processedBytes;
        result["totalBytes"] = rt->totalBytes;
    }
    result["resources"] = resources(id);
    if (!task.errorCode.empty()) result["suggestion"] = errorSuggestion(task.errorCode);
    return result;
}

json TaskManager::resources(const std::string& id) {
    json stages = json::array();
    int64_t cpuMs = 0, durationMs = 0;
    uint64_t peakMemory = 0, readBytes = 0, writeBytes = 0;
    for (const auto& row : db_.query("SELECT * FROM task_resources WHERE task_id=? ORDER BY updated_at ASC", {id})) {
        const int64_t stageCpu = row.value("cpu_ms", 0ll);
        const int64_t stageDuration = row.value("duration_ms", 0ll);
        const uint64_t stagePeak = static_cast<uint64_t>(row.value("peak_memory", 0ll));
        const uint64_t stageRead = static_cast<uint64_t>(row.value("read_bytes", 0ll));
        const uint64_t stageWrite = static_cast<uint64_t>(row.value("write_bytes", 0ll));
        cpuMs += stageCpu;
        durationMs += stageDuration;
        peakMemory = std::max(peakMemory, stagePeak);
        readBytes += stageRead;
        writeBytes += stageWrite;
        stages.push_back(json{{"stage", row.value("stage", "")}, {"stageLabel", stageLabel(row.value("stage", ""))},
                              {"cpuMs", stageCpu}, {"durationMs", stageDuration},
                              {"peakMemory", stagePeak}, {"readBytes", stageRead}, {"writeBytes", stageWrite},
                              {"samples", row.value("samples", 0)}, {"updatedAt", row.value("updated_at", "")}});
    }
    const double cpuPercent = durationMs > 0
        ? std::round(static_cast<double>(cpuMs) / static_cast<double>(durationMs) * 1000.0) / 10.0 : 0.0;
    return json{{"stages", stages}, {"cpuMs", cpuMs}, {"durationMs", durationMs},
                {"peakMemory", peakMemory}, {"readBytes", readBytes}, {"writeBytes", writeBytes},
                {"cpuPercent", cpuPercent}, {"sampled", !stages.empty()}};
}

bool TaskManager::cancelTask(const std::string& id, std::string& error) {
    auto rt = findRuntime(id);
    if (rt && (rt->task.status == "RUNNING" || rt->task.status == "QUEUED")) {
        rt->cancel.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (rt->task.status == "QUEUED") {
                rt->task.status = "CANCELED";
                rt->task.stage = "CANCELED";
                rt->task.finishedAt = nowMs();
                rt->task.elapsedMs = rt->task.finishedAt - rt->task.createdAt;
                rt->task.errorCode = "CANCELLED";
                rt->task.errorMessage = "用户取消了任务";
                persistTask(rt->task);
            }
        }
        events_.publish(json{{"type", "task.canceling"}, {"taskId", id}});
        if (rt->task.status == "QUEUED") {
            appendLog(rt, "WARNING", "任务已取消。");
            events_.publish(json{{"type", "task.canceled"}, {"taskId", id}});
        }
        return true;
    }
    const auto rows = db_.query("SELECT status FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) { error = "任务不存在"; return false; }
    const std::string status = rows[0].value("status", "");
    if (!isTerminalStatus(status)) {
        db_.query("UPDATE conversion_tasks SET status='CANCELED',stage='CANCELED',error_code='CANCELLED',"
                  "error_message='用户取消了任务',finished_at=? WHERE id=?", {nowMs(), id});
        return true;
    }
    error = "任务已经结束，无法取消";
    return false;
}

bool TaskManager::pauseTask(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) { error = "任务不存在"; return false; }
    if (rows[0].value("status", "") != "QUEUED") {
        error = "只能暂停排队中的任务；运行中的外部进程不支持挂起";
        return false;
    }
    db_.query("UPDATE conversion_tasks SET status='PAUSED',stage='PAUSED' WHERE id=?", {id});
    auto rt = findRuntime(id);
    if (rt) { std::lock_guard<std::mutex> lock(mutex_); rt->task.status = "PAUSED"; rt->task.stage = "PAUSED"; }
    events_.publish(json{{"type", "task.status"}, {"taskId", id}, {"status", "PAUSED"}});
    return true;
}

bool TaskManager::resumeTask(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) { error = "任务不存在"; return false; }
    if (rows[0].value("status", "") != "PAUSED") { error = "任务不在暂停状态"; return false; }
    db_.query("UPDATE conversion_tasks SET status='QUEUED',stage='QUEUED' WHERE id=?", {id});
    auto rt = findRuntime(id);
    if (rt) { std::lock_guard<std::mutex> lock(mutex_); rt->task.status = "QUEUED"; rt->task.stage = "QUEUED"; }
    events_.publish(json{{"type", "task.status"}, {"taskId", id}, {"status", "QUEUED"}});
    cv_.notify_all();
    return true;
}

bool TaskManager::removeTask(const std::string& id, bool deleteLogs, std::string& error) {
    auto rt = findRuntime(id);
    if (rt && !isTerminalStatus(rt->task.status)) { error = "运行中的任务不能删除，请先取消"; return false; }
    const auto rows = db_.query("SELECT status,logs_path FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) { error = "任务不存在"; return false; }
    if (!isTerminalStatus(rows[0].value("status", ""))) { error = "只能删除已结束的任务"; return false; }
    db_.query("DELETE FROM conversion_tasks WHERE id=?", {id});
    if (deleteLogs) {
        std::error_code ec;
        fs::remove(pathFromUtf8(rows[0].value("logs_path", "")), ec);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtime_.erase(id);
    }
    return true;
}

bool TaskManager::setPriority(const std::string& id, int priority, std::string& error) {
    const auto rows = db_.query("SELECT status FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) { error = "任务不存在"; return false; }
    if (rows[0].value("status", "") != "QUEUED" && rows[0].value("status", "") != "PAUSED") {
        error = "只能调整排队中任务的优先级";
        return false;
    }
    priority = std::max(-100, std::min(100, priority));
    db_.query("UPDATE conversion_tasks SET priority=? WHERE id=?", {priority, id});
    auto rt = findRuntime(id);
    if (rt) { std::lock_guard<std::mutex> lock(mutex_); rt->task.priority = priority; }
    cv_.notify_all();
    return true;
}

bool TaskManager::retryTask(const std::string& id, std::string& newIdOut, std::string& error) {
    const auto rows = db_.query("SELECT params_json FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) { error = "任务不存在"; return false; }
    ConversionParams params;
    try { params = ConversionParams::fromJson(json::parse(rows[0].value("params_json", "{}"))); }
    catch (...) { error = "任务参数损坏"; return false; }
    params.overwrite = true;
    std::string code;
    if (!createTask(params, newIdOut, code, error)) return false;
    return true;
}

json TaskManager::logs(const std::string& id, int afterSeq, int limit, const std::string& level) {
    const fs::path path = logFilePath(id);
    std::ifstream input(path.u8string(), std::ios::binary);
    if (!input) return json{{"items", json::array()}, {"path", pathToUtf8(path)}};
    std::deque<json> items;
    std::string line;
    limit = std::max(1, std::min(limit, 5000));
    const std::string targetLevel = toUpper(level);
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        try {
            json entry = json::parse(line);
            if (afterSeq > 0 && entry.value("seq", 0ll) <= afterSeq) continue;
            if (!targetLevel.empty() && targetLevel != "ALL" && entry.value("level", "") != targetLevel) continue;
            items.push_back(entry);
            while (static_cast<int>(items.size()) > limit) items.pop_front();
        } catch (...) {}
    }
    return json{{"items", items}, {"path", pathToUtf8(path)}};
}

fs::path TaskManager::logFilePath(const std::string& id) const {
    return settings_.logsDir() / (id + ".log.jsonl");
}

json TaskManager::result(const std::string& id) {
    const auto rows = db_.query("SELECT * FROM conversion_tasks WHERE id=?", {id});
    if (rows.empty()) return nullptr;
    TaskRecord task = taskFromRow(rows[0]);
    if (task.status != "SUCCEEDED") return json{{"ready", false}, {"status", task.status}, {"errorCode", task.errorCode}};
    json output{
        {"ready", true}, {"id", task.id}, {"modelName", task.modelName}, {"outputPath", task.outputPath},
        {"outputSize", task.outputSize}, {"quantType", task.params.quantType.empty() ? toLower(task.params.outType) : task.params.quantType},
        {"sha256", task.sha256}, {"elapsedMs", task.elapsedMs},
        {"reportPath", pathToUtf8(settings_.dataDir() / "reports" / (task.id + ".json"))}
    };
    try { output["verification"] = json::parse(task.verificationJson); } catch (...) {}
    return output;
}

json TaskManager::queueStats() {
    json stats{{"running", 0}, {"queued", 0}, {"paused", 0}, {"failed", 0}, {"succeeded", 0}, {"canceled", 0}};
    for (const auto& row : db_.query("SELECT status,COUNT(*) AS n FROM conversion_tasks GROUP BY status")) {
        const std::string status = row.value("status", "");
        if (status == "RUNNING") stats["running"] = row["n"];
        else if (status == "QUEUED") stats["queued"] = row["n"];
        else if (status == "PAUSED") stats["paused"] = row["n"];
        else if (status == "FAILED") stats["failed"] = row["n"];
        else if (status == "SUCCEEDED") stats["succeeded"] = row["n"];
        else if (status == "CANCELED") stats["canceled"] = row["n"];
    }
    stats["maxConcurrent"] = settings_.get().maxConcurrent;
    return stats;
}

void TaskManager::setConcurrency(int value) {
    value = std::max(1, std::min(8, value));
    std::string error;
    settings_.update(json{{"maxConcurrent", value}}, error);
    cv_.notify_all();
}

std::string TaskManager::ensureModelRecord(const ModelScanResult& scan) {
    const auto existing = db_.query("SELECT id FROM models WHERE path=? LIMIT 1", {scan.path});
    if (!existing.empty()) return existing[0].value("id", "");
    const std::string id = newId("model");
    const json compatibility = converter_.compatibility(scan);
    const json analysis = ModelScanner::buildInspectionReport(scan);
    std::string error;
    db_.query("INSERT INTO models(id,name,path,architecture,architecture_family,parameter_count,dtype,total_size,shard_count,"
              "tensor_count,status,supported,favorite,notes,tags,analysis_json,config_json,created_at,updated_at) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,'','',?,?,?,?)",
              {id, scan.name, scan.path, scan.architecture, scan.architectureFamily,
               static_cast<int64_t>(scan.parameterCount), scan.dtype, static_cast<int64_t>(scan.totalSize),
               scan.shardCount, scan.tensorCount, scan.complete ? "READY" : "INCOMPLETE",
               compatibility.value("supported", false) ? 1 : 0, analysis.dump(), scan.config.dump(), isoNow(), isoNow()}, &error);
    for (const auto& file : scan.files) {
        db_.query("INSERT INTO model_files(model_id,path,file_name,size,kind,valid,details_json) VALUES(?,?,?,?,?,?,?)",
                  {id, file.value("path", ""), file.value("name", ""), file.value("size", 0ll),
                   file.value("kind", ""), file.value("valid", true) ? 1 : 0, file.dump()});
    }
    return id;
}

} // namespace mf
