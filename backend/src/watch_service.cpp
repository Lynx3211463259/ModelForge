#include "modelforge/watch_service.hpp"

#include "modelforge/logger.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <chrono>
#include <set>

namespace mf {

namespace {
bool isModelDirectory(const fs::path& dir, std::string& reason) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        reason = "不是目录";
        return false;
    }
    const fs::path config = dir / "config.json";
    if (!fs::is_regular_file(config, ec)) {
        reason = "缺少 config.json";
        return false;
    }
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (toLower(it->path().extension().string()) == ".safetensors") return true;
    }
    reason = "没有 .safetensors 文件";
    return false;
}

int64_t pathTimeSeconds(const fs::path& path) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count());
}
} // namespace

WatchService::WatchService(Database& db, SettingsService& settings, SystemService& system, TaskManager& tasks,
                           EventHub& events)
    : db_(db), settings_(settings), system_(system), tasks_(tasks), events_(events) {}

WatchService::~WatchService() { stop(); }

void WatchService::start() {
    if (loop_.joinable()) return;
    stop_.store(false);
    loop_ = std::thread([this] { loopMain(); });
}

void WatchService::stop() {
    stop_.store(true);
    if (loop_.joinable()) loop_.join();
}

void WatchService::notify() { wake_.store(true); }

void WatchService::loopMain() {
    while (!stop_.load()) {
        const AppSettings s = settings_.get();
        const int interval = std::max(5, s.watchIntervalSec);
        const int64_t now = nowMs();
        const bool due = lastScanAt_ == 0 || (now - lastScanAt_) >= static_cast<int64_t>(interval) * 1000;
        if (s.folderWatchEnabled && !s.watchFolders.empty() && due) {
            wake_.store(false);
            std::string error;
            try {
                scan(false, error);
            } catch (const std::exception& ex) {
                lastError_ = ex.what();
                Logger::instance().warn("[watch] 扫描失败: " + lastError_);
            }
            if (!error.empty()) lastError_ = error;
        }
        for (int i = 0; i < 10 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::string WatchService::signatureFor(const fs::path& dir) const {
    std::error_code ec;
    uint64_t total = 0;
    int64_t newest = 0;
    int counts = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = toLower(it->path().extension().string());
        if (ext != ".safetensors" && it->path().filename() != "config.json") continue;
        total += static_cast<uint64_t>(fs::file_size(it->path(), ec));
        newest = std::max(newest, pathTimeSeconds(it->path()));
        ++counts;
    }
    return std::to_string(total) + ":" + std::to_string(newest) + ":" + std::to_string(counts);
}

json WatchService::seenRow(const std::string& path) const {
    const auto rows = db_.query("SELECT * FROM watch_seen WHERE path=?", {path});
    return rows.empty() ? json(nullptr) : rows[0];
}

void WatchService::remember(const std::string& path, const std::string& signature, const std::string& modelId,
                            const std::string& taskId) {
    db_.query("INSERT OR REPLACE INTO watch_seen(path,signature,model_id,task_id,updated_at) VALUES(?,?,?,?,?)",
              {path, signature, modelId, taskId, isoNow()});
}

void WatchService::recordEvent(const std::string& folder, const std::string& path, const std::string& action,
                               const std::string& modelId, const std::string& taskId, const std::string& detail) {
    db_.query("INSERT INTO watch_events(id,folder,path,action,model_id,task_id,detail,created_at) VALUES(?,?,?,?,?,?,?,?)",
              {newId("watch"), folder, path, action, modelId, taskId, sanitizeSingleLine(detail, 1000), isoNow()});
    events_.publish(json{{"type", "watch"}, {"action", action}, {"path", path}, {"detail", detail}});
}

bool WatchService::handleCandidate(const fs::path& dir, const std::string& folder, bool force, json& summary) {
    const std::string path = pathToUtf8(dir);
    const std::string signature = signatureFor(dir);
    const json seen = seenRow(path);
    if (!force && seen.is_object() && seen.value("signature", "") == signature) {
        summary["skipped"] = summary.value("skipped", 0) + 1;
        return false;
    }

    std::string code;
    std::string error;
    const json model = system_.importModel(path, code, error);
    if (model.is_null()) {
        remember(path, signature, "", "");
        recordEvent(folder, path, "FAILED", "", "", error.empty() ? code : error);
        summary["failed"] = summary.value("failed", 0) + 1;
        summary["errors"].push_back(json{{"path", path}, {"code", code}, {"message", error}});
        return false;
    }

    const std::string modelId = model.value("id", "");
    recordEvent(folder, path, "IMPORTED", modelId, "",
                "导入模型 " + model.value("name", "") + "（" + std::to_string(model.value("shardCount", 0)) + " 个分片）");
    summary["imported"] = summary.value("imported", 0) + 1;
    summary["modelIds"].push_back(modelId);

    const AppSettings s = settings_.get();
    std::string taskId;
    if (s.watchAutoConvert && !s.watchPresetId.empty()) {
        json params = json::object();
        const auto rows = db_.query("SELECT params_json FROM quantization_profiles WHERE id=?", {s.watchPresetId});
        if (!rows.empty()) {
            try { params = json::parse(rows[0].value("params_json", "{}")); } catch (...) {}
        }
        if (rows.empty()) {
            recordEvent(folder, path, "CONVERT_SKIPPED", modelId, "", "预设 " + s.watchPresetId + " 已不存在");
        } else {
            ConversionParams conversion;
            conversion.modelId = modelId;
            conversion.inputPath = model.value("path", path);
            conversion.outputDir = s.defaultOutputDir;
            const std::string quant = params.value("quantType", std::string(""));
            conversion.outputName = sanitizeFileName(model.value("name", "model")) + (quant.empty() ? "" : "-" + toLower(quant));
            conversion.outType = params.value("outType", std::string("f16"));
            conversion.quantType = quant;
            conversion.threads = params.value("threads", s.threads);
            conversion.verify = params.value("verify", true);
            conversion.computeSha256 = params.value("computeSha256", true);
            conversion.overwrite = false;
            std::string taskCode, taskError;
            if (tasks_.createTask(conversion, taskId, taskCode, taskError)) {
                summary["queued"] = summary.value("queued", 0) + 1;
                recordEvent(folder, path, "CONVERT_QUEUED", modelId, taskId,
                            "已按预设 " + s.watchPresetId + " 创建转换任务");
            } else {
                recordEvent(folder, path, "CONVERT_FAILED", modelId, "",
                            taskError.empty() ? taskCode : taskError);
            }
        }
    }
    remember(path, signature, modelId, taskId);
    return true;
}

json WatchService::scanFolders(bool force) {
    const int64_t started = nowMs();
    const AppSettings s = settings_.get();
    json summary{{"scannedAt", isoNow()}, {"imported", 0}, {"skipped", 0}, {"queued", 0}, {"failed", 0},
                 {"folders", json::array()}, {"errors", json::array()}, {"modelIds", json::array()}};
    for (const auto& folderValue : s.watchFolders) {
        if (folderValue.empty()) continue;
        const fs::path folder = pathFromUtf8(folderValue);
        std::error_code ec;
        json entry{{"path", folderValue}, {"exists", fs::is_directory(folder, ec)},
                   {"candidates", 0}, {"imported", 0}, {"skipped", 0}, {"queued", 0}, {"failed", 0},
                   {"errors", json::array()}};
        if (!fs::is_directory(folder, ec)) {
            entry["error"] = "目录不存在或不可访问";
            summary["folders"].push_back(entry);
            continue;
        }
        std::vector<fs::path> candidates;
        for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_directory(ec)) continue;
            std::string reason;
            if (isModelDirectory(it->path(), reason)) candidates.push_back(it->path());
        }
        std::sort(candidates.begin(), candidates.end());
        entry["candidates"] = candidates.size();
        for (const auto& candidate : candidates) {
            json before = summary;
            handleCandidate(candidate, folderValue, force, summary);
            for (const char* key : {"imported", "skipped", "queued", "failed"}) {
                const int delta = summary.value(key, 0) - before.value(key, 0);
                if (delta) entry[key] = entry.value(key, 0) + delta;
            }
            if (summary["errors"].size() > before["errors"].size()) {
                entry["errors"].push_back(summary["errors"].back());
            }
        }
        summary["folders"].push_back(entry);
        const std::string error = entry.value("error", "");
        if (!error.empty()) {
            summary["errors"].push_back(json{{"path", folderValue}, {"code", "FOLDER_UNAVAILABLE"}, {"message", error}});
        }
    }
    summary["durationMs"] = nowMs() - started;
    return summary;
}

json WatchService::scan(bool force, std::string& error) {
    std::lock_guard<std::mutex> lock(scanMutex_);
    const AppSettings s = settings_.get();
    if (s.watchFolders.empty()) {
        error = "尚未配置监控目录";
        return nullptr;
    }
    const int64_t started = nowMs();
    json summary = scanFolders(force);
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        lastScanAt_ = started;
        lastScanDurationMs_ = nowMs() - started;
        lastError_ = summary.value("failed", 0) > 0 ? "部分目录导入失败" : "";
    }
    if (summary.value("imported", 0) > 0 || summary.value("queued", 0) > 0) {
        Logger::instance().info("[watch] 导入 " + std::to_string(summary.value("imported", 0)) +
                                " 个模型，排队 " + std::to_string(summary.value("queued", 0)) + " 个转换任务");
    }
    return summary;
}

json WatchService::status() {
    const AppSettings s = settings_.get();
    int64_t scanAt = 0;
    int64_t scanDuration = 0;
    std::string scanError;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        scanAt = lastScanAt_;
        scanDuration = lastScanDurationMs_;
        scanError = lastError_;
    }
    json folders = json::array();
    std::error_code ec;
    for (const auto& folderValue : s.watchFolders) {
        json entry{{"path", folderValue}, {"exists", fs::is_directory(pathFromUtf8(folderValue), ec)}};
        // watch_seen stores the path exactly as the scanner reports it (backslashes on
        // Windows), while the setting may use forward slashes.
        std::string prefix = folderValue;
#ifdef _WIN32
        std::replace(prefix.begin(), prefix.end(), '/', '\\');
#endif
        const auto seen = db_.query("SELECT COUNT(*) AS n FROM watch_seen WHERE path LIKE ?",
                                    {prefix + "%"});
        entry["known"] = seen.empty() ? 0 : seen[0].value("n", 0);
        const auto last = db_.query(
            "SELECT created_at FROM watch_events WHERE folder=? ORDER BY created_at DESC LIMIT 1", {folderValue});
        entry["lastEventAt"] = last.empty() ? "" : last[0].value("created_at", "");
        folders.push_back(entry);
    }
    int64_t nextScanAt = 0;
    if (s.folderWatchEnabled && scanAt > 0) {
        nextScanAt = scanAt + static_cast<int64_t>(std::max(5, s.watchIntervalSec)) * 1000;
    }
    json stats = json::object();
    const auto counts = db_.query(
        "SELECT action, COUNT(*) AS n FROM watch_events GROUP BY action");
    for (const auto& row : counts) stats[row.value("action", "")] = row.value("n", 0);

    return json{{"enabled", s.folderWatchEnabled},
                {"intervalSec", s.watchIntervalSec},
                {"autoConvert", s.watchAutoConvert},
                {"presetId", s.watchPresetId},
                {"folders", folders},
                {"lastScanAt", scanAt > 0 ? isoFromMs(scanAt) : ""},
                {"lastScanDurationMs", scanDuration},
                {"nextScanAt", nextScanAt > 0 ? isoFromMs(nextScanAt) : ""},
                {"lastError", scanError},
                {"stats", stats},
                {"events", events(20)}};
}

json WatchService::events(int limit) {
    json items = json::array();
    limit = std::max(1, std::min(limit, 500));
    for (const auto& row : db_.query("SELECT * FROM watch_events ORDER BY created_at DESC LIMIT ?", {limit})) {
        items.push_back(json{{"id", row.value("id", "")}, {"folder", row.value("folder", "")},
                             {"path", row.value("path", "")}, {"action", row.value("action", "")},
                             {"modelId", row.value("model_id", "")}, {"taskId", row.value("task_id", "")},
                             {"detail", row.value("detail", "")},
                             {"createdAt", row.value("created_at", "")}});
    }
    return items;
}

} // namespace mf
