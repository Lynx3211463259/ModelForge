#include "modelforge/download_manager.hpp"

#include "modelforge/crypto.hpp"
#include "modelforge/gguf_reader.hpp"
#include "modelforge/hf_client.hpp"
#include "modelforge/http_client.hpp"
#include "modelforge/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mf {

namespace {

constexpr int kMaxFileRetries = 5;
constexpr int kMaxConcurrency = 16;

int64_t fileSizeOrZero(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return 0;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<int64_t>(size);
}

struct LocalFileState {
    int64_t id = 0;
    std::string remotePath;
    std::string localPath;
    uint64_t size = 0;
    uint64_t downloaded = 0;
    std::string checksum;
    std::string etag;
    std::string lastModified;
    std::string status;
    std::string error;
};

} // namespace

struct DownloadManager::Job {
    std::string id;
    std::string repoId;
    std::string revision;
    std::string destination;
    int concurrency = 4;
    bool verifyChecksum = true;
    bool autoImport = true;
    std::string token;
    std::atomic<bool> abort{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<SpeedLimiter> limiter = std::make_shared<SpeedLimiter>();
    std::thread runner;
    std::mutex filesMutex;
    std::vector<LocalFileState> files;
    std::set<int64_t> completedIds;
    std::vector<LocalFileState> pendingFiles;
    uint64_t initialDoneBytes = 0;
    int initialCompletedFiles = 0;
    std::set<std::string> active;
    uint64_t totalBytes = 0;
    uint64_t downloadedBytes = 0;
    int completedFiles = 0;
    double speed = 0;
    std::string currentFiles;
    std::string firstError;
    std::string firstErrorCode;
};

namespace {

void waitWhilePaused(const std::shared_ptr<DownloadManager::Job>& job) {
    std::unique_lock<std::mutex> lock(job->mutex);
    while (job->paused.load() && !job->cancelled.load()) {
        job->abort.store(false);
        job->cv.wait_for(lock, std::chrono::milliseconds(200));
    }
}

bool waitInterruptible(const std::shared_ptr<DownloadManager::Job>& job, int totalMs) {
    const int slices = std::max(1, totalMs / 100);
    for (int i = 0; i < slices; ++i) {
        if (job->cancelled.load()) return false;
        waitWhilePaused(job);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !job->cancelled.load();
}

std::string statusLabel(const std::string& status) {
    if (status == "pending") return "等待中";
    if (status == "resolving") return "解析中";
    if (status == "downloading") return "下载中";
    if (status == "paused") return "已暂停";
    if (status == "retrying") return "重试中";
    if (status == "verifying") return "校验中";
    if (status == "completed") return "已完成";
    if (status == "failed") return "失败";
    if (status == "cancelled") return "已取消";
    return status;
}

} // namespace

DownloadManager::DownloadManager(Database& db, SettingsService& settings, SystemService& system, EventHub& events)
    : db_(db), settings_(settings), system_(system), events_(events) {}

DownloadManager::~DownloadManager() { stop(); }

std::string DownloadManager::loadSetting(const std::string& key, const std::string& fallback) const {
    const auto rows = db_.query("SELECT value FROM download_settings WHERE key=?", {key});
    if (rows.empty()) return fallback;
    return rows[0].value("value", fallback);
}

void DownloadManager::saveSetting(const std::string& key, const std::string& value) {
    db_.query("INSERT INTO download_settings(key,value,updated_at) VALUES(?,?,?) "
              "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at",
              {key, value, isoNow()});
}

json DownloadManager::settings() {
    int concurrency = 4;
    try { concurrency = std::stoi(loadSetting("concurrency", "4")); } catch (...) {}
    uint64_t maxSpeed = 0;
    try { maxSpeed = std::stoull(loadSetting("max_speed", "0")); } catch (...) {}
    return json{
        {"concurrency", std::max(1, std::min(concurrency, kMaxConcurrency))},
        {"maxSpeed", maxSpeed},
        {"verifyChecksum", loadSetting("verify_checksum", "1") == "1"},
        {"autoImport", loadSetting("auto_import", "1") == "1"},
        {"autoResume", loadSetting("auto_resume", "0") == "1"},
        {"defaultDestination", pathToUtf8(settings_.modelsDir())},
        {"tokenSet", hasToken()},
        {"tokenMasked", tokenStatus().value("masked", "")},
        {"engine", "WinHTTP + Hugging Face REST API"}
    };
}

bool DownloadManager::updateSettings(const json& patch, std::string& error) {
    if (patch.contains("concurrency")) {
        int value = patch.value("concurrency", 4);
        if (value < 1 || value > kMaxConcurrency) {
            error = "并发数量必须在 1 到 " + std::to_string(kMaxConcurrency) + " 之间";
            return false;
        }
        saveSetting("concurrency", std::to_string(value));
    }
    if (patch.contains("maxSpeed")) {
        uint64_t value = patch.value("maxSpeed", 0ull);
        saveSetting("max_speed", std::to_string(value));
    }
    if (patch.contains("verifyChecksum")) saveSetting("verify_checksum", patch.value("verifyChecksum", true) ? "1" : "0");
    if (patch.contains("autoImport")) saveSetting("auto_import", patch.value("autoImport", true) ? "1" : "0");
    if (patch.contains("autoResume")) saveSetting("auto_resume", patch.value("autoResume", false) ? "1" : "0");
    if (patch.contains("token") && patch["token"].is_string()) {
        setToken(patch["token"].get<std::string>(), error);
        if (!error.empty()) return false;
    }
    return true;
}

std::string DownloadManager::protectSecret(const std::string& plain, bool& ok) {
    ok = false;
#ifdef _WIN32
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"ModelForge HF token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::string();
    }
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    ok = true;
    return base64Encode(result);
#else
    // No DPAPI on POSIX. Keep the token out of the SQLite database entirely and store
    // it in a file readable only by the current user (mode 0600).
    const fs::path dir = settings_.dataDir() / "secrets";
    std::string dirError;
    if (!ensureDirectory(dir, dirError)) return std::string();
    const fs::path file = dir / "hf.token";
    if (!writeTextFile(file, plain)) return std::string();
    chmod(file.c_str(), 0600);
    ok = true;
    return "posix-file:hf.token";
#endif
}

std::string DownloadManager::unprotectSecret(const std::string& stored, bool& ok) const {
    ok = false;
#ifdef _WIN32
    const std::string raw = base64Decode(stored);
    if (raw.empty()) return std::string();
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(raw.data()));
    input.cbData = static_cast<DWORD>(raw.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::string();
    }
    std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    ok = true;
    return result;
#else
    if (startsWith(stored, "posix-file:")) {
        const fs::path file = settings_.dataDir() / "secrets" / stored.substr(11);
        const std::string value = trim(readTextFile(file, 16384));
        ok = !value.empty();
        return value;
    }
    return std::string();
#endif
}

bool DownloadManager::hasToken() const { return !token().empty(); }

std::string DownloadManager::token() const {
    const std::string stored = loadSetting("hf_token", "");
    if (stored.empty()) return std::string();
    bool ok = false;
    std::string value = unprotectSecret(stored, ok);
    if (!ok) {
        // Legacy plaintext value (only used when DPAPI is unavailable).
        if (startsWith(stored, "plain:")) value = stored.substr(6);
    }
    return value;
}

void DownloadManager::setToken(const std::string& value, std::string& error) {
    const std::string trimmed = trim(value);
    if (trimmed.empty()) {
        db_.query("DELETE FROM download_settings WHERE key='hf_token'");
        return;
    }
    if (!startsWith(trimmed, "hf_")) {
        error = "Token 格式不正确，Hugging Face Token 通常以 hf_ 开头";
        return;
    }
    bool ok = false;
    std::string stored = protectSecret(trimmed, ok);
    if (ok) {
        saveSetting("hf_token", stored);
#ifdef _WIN32
        saveSetting("token_scheme", "dpapi");
#else
        saveSetting("token_scheme", "posix-file");
#endif
    } else {
        saveSetting("hf_token", "plain:" + trimmed);
        saveSetting("token_scheme", "plaintext");
        Logger::instance().warn("Hugging Face Token 以明文形式存储（无法创建受限凭据文件，请检查 data/secrets 目录权限）");
    }
}

json DownloadManager::tokenStatus() {
    const std::string value = token();
    std::string masked;
    if (!value.empty()) {
        masked = value.size() > 8 ? value.substr(0, 6) + "…" + value.substr(value.size() - 4) : "已配置";
    }
    return json{{"configured", !value.empty()}, {"masked", masked},
                {"scheme", loadSetting("token_scheme", "dpapi")}};
}

bool DownloadManager::tryAcquireLock() {
    if (ownsLock_.load()) return true;
    std::error_code ec;
    const fs::path dir = settings_.dataDir();
    fs::create_directories(dir, ec);
    const fs::path lockPath = dir / "downloads.lock";
#ifdef _WIN32
    HANDLE handle = CreateFileW(lockPath.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    lockHandle_ = handle;
#else
    const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    lockFd_ = fd;
#endif
    ownsLock_.store(true);
    Logger::instance().info("Download scheduler lock acquired");
    return true;
}

void DownloadManager::releaseLock() {
    if (!ownsLock_.exchange(false)) return;
#ifdef _WIN32
    if (lockHandle_) {
        CloseHandle(static_cast<HANDLE>(lockHandle_));
        lockHandle_ = nullptr;
    }
#else
    if (lockFd_ >= 0) {
        flock(lockFd_, LOCK_UN);
        ::close(lockFd_);
        lockFd_ = -1;
    }
#endif
}

void DownloadManager::recoverInterruptedTasks() {
    const auto rows = db_.query(
        "SELECT id,status FROM download_tasks WHERE status IN ('resolving','downloading','retrying','verifying')");
    if (rows.empty()) return;
    for (const auto& row : rows) {
        const std::string id = row.value("id", "");
        db_.query("UPDATE download_tasks SET status='paused', speed=0, current_files='', updated_at=?, "
                  "error_message='应用重启，下载断点已保留，可点击继续恢复' WHERE id=?",
                  {nowMs(), id});
        appendLog(id, "WARNING", "应用重启：任务从 " + row.value("status", "") + " 转为暂停，断点保留");
    }
    Logger::instance().info("Recovered " + std::to_string(rows.size()) + " interrupted download task(s)");
    const bool autoResume = loadSetting("auto_resume", "0") == "1";
    if (autoResume) {
        for (const auto& row : rows) {
            const std::string id = row.value("id", "");
            db_.query("UPDATE download_tasks SET status='pending', error_message='' WHERE id=?", {id});
            appendLog(id, "INFO", "已按设置自动恢复下载任务");
        }
    }
}

void DownloadManager::start() {
    if (loop_.joinable()) return;
    stop_.store(false);
    loop_ = std::thread([this] { schedulerLoop(); });
}

void DownloadManager::schedulerLoop() {
    auto lastRecover = false;
    while (!stop_.load()) {
        if (!ownsLock_.load()) {
            if (!tryAcquireLock()) {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(1500), [&] { return stop_.load(); });
                continue;
            }
        }
        if (!lastRecover) {
            recoverInterruptedTasks();
            lastRecover = true;
        }
        // Start any pending task that has no in-memory job yet (also picks up CLI-created tasks).
        const auto pending = db_.query(
            "SELECT id FROM download_tasks WHERE status IN ('pending','downloading') ORDER BY created_at ASC LIMIT 8");
        for (const auto& row : pending) {
            const std::string id = row.value("id", "");
            bool known = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                known = jobs_.count(id) > 0;
            }
            if (!known) startJob(id);
        }
        // Apply control requests issued by another process (CLI) or the Web UI.
        std::vector<std::pair<std::string, std::shared_ptr<Job>>> activeJobs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& item : jobs_) activeJobs.push_back({item.first, item.second});
        }
        for (auto& item : activeJobs) {
            const auto rows = db_.query("SELECT status FROM download_tasks WHERE id=?", {item.first});
            if (rows.empty()) continue;
            const std::string dbStatus = rows[0].value("status", "");
            auto& job = item.second;
            if (dbStatus == "paused" && !job->paused.load()) {
                job->paused.store(true);
                job->abort.store(true);
                job->cv.notify_all();
            } else if (dbStatus == "cancelled" && !job->cancelled.load()) {
                job->cancelled.store(true);
                job->paused.store(false);
                job->abort.store(true);
                job->cv.notify_all();
            } else if (dbStatus == "downloading" && job->paused.load()) {
                job->paused.store(false);
                job->abort.store(false);
                job->cv.notify_all();
            }
        }
        // Reap finished jobs.
        std::vector<std::shared_ptr<Job>> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = jobs_.begin(); it != jobs_.end();) {
                if (it->second->finished.load()) {
                    finished.push_back(it->second);
                    it = jobs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& job : finished) {
            if (job->runner.joinable()) job->runner.join();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(1000), [&] { return stop_.load(); });
    }
}

void DownloadManager::startJob(const std::string& id) {
    auto job = std::make_shared<Job>();
    job->id = id;
    const auto rows = db_.query("SELECT * FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) return;
    const json row = rows[0];
    job->repoId = row.value("repo_id", "");
    job->revision = row.value("revision", "main");
    job->destination = row.value("destination", "");
    job->concurrency = std::max(1, std::min(row.value("concurrency", 4), kMaxConcurrency));
    job->verifyChecksum = loadSetting("verify_checksum", "1") == "1";
    job->autoImport = loadSetting("auto_import", "1") == "1";
    job->token = token();
    const uint64_t maxSpeed = row.value("max_speed", 0ull);
    job->limiter->setRate(maxSpeed);
    db_.query("UPDATE download_tasks SET status='downloading', error_message='', updated_at=? WHERE id=?", {nowMs(), id});
    appendLog(id, "INFO", "开始下载 " + job->repoId + "@" + job->revision + " → " + job->destination);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_[id] = job;
    }
    job->runner = std::thread([this, job] { runJob(job); });
}

void DownloadManager::pullFilesIntoJob(const std::shared_ptr<Job>& job, std::string& error) {
    const auto rows = db_.query("SELECT * FROM download_files WHERE task_id=? ORDER BY id", {job->id});
    std::lock_guard<std::mutex> lock(job->filesMutex);
    job->files.clear();
    job->completedIds.clear();
    job->initialDoneBytes = 0;
    job->initialCompletedFiles = 0;
    uint64_t total = 0;
    for (const auto& row : rows) {
        LocalFileState file;
        file.id = row.value("id", 0ll);
        file.remotePath = row.value("remote_path", "");
        file.localPath = row.value("local_path", "");
        file.size = row.value("file_size", 0ull);
        file.downloaded = row.value("downloaded_size", 0ull);
        file.checksum = row.value("checksum", "");
        file.etag = row.value("etag", "");
        file.lastModified = row.value("last_modified", "");
        file.status = row.value("status", "pending");
        total += file.size;
        if (file.status == "completed") {
            job->completedIds.insert(file.id);
            job->initialDoneBytes += file.size;
            ++job->initialCompletedFiles;
        }
        job->files.push_back(std::move(file));
    }
    job->totalBytes = total;
    // Skip files that are already complete so workers only handle the remainder.
    std::vector<LocalFileState> pendingOnly;
    for (const auto& file : job->files) {
        if (!job->completedIds.count(file.id)) pendingOnly.push_back(file);
    }
    job->pendingFiles = pendingOnly;
    if (job->pendingFiles.empty() && !rows.empty()) {
        error = "所有文件已下载完成";
    } else if (rows.empty()) {
        error = "没有待下载文件";
    }
}

void DownloadManager::runJob(std::shared_ptr<Job> job) {
    ScopeExit finish([&] {
        job->finished.store(true);
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    });

    std::string error;
    pullFilesIntoJob(job, error);
    if (!error.empty()) appendLog(job->id, "WARNING", error + "，直接进入校验阶段");

    std::atomic<size_t> nextIndex{0};
    std::atomic<int> activeWorkers{0};
    std::vector<std::thread> workers;
    const size_t workerCount = std::min<size_t>(static_cast<size_t>(job->concurrency),
                                                std::max<size_t>(1, job->pendingFiles.size()));
    activeWorkers.store(static_cast<int>(workerCount));
    for (size_t w = 0; w < workerCount; ++w) {
        workers.emplace_back([this, job, &nextIndex, &activeWorkers] {
            ScopeExit leave([&] { activeWorkers.fetch_sub(1); });
            while (!job->cancelled.load()) {
                waitWhilePaused(job);
                if (job->cancelled.load()) break;
                const size_t index = nextIndex.fetch_add(1);
                if (index >= job->pendingFiles.size()) break;
                LocalFileState file;
                {
                    std::lock_guard<std::mutex> lock(job->filesMutex);
                    file = job->pendingFiles[index];
                }
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->active.insert(file.remotePath);
                }
                db_.query("UPDATE download_files SET status='downloading', updated_at=? WHERE id=?", {nowMs(), file.id});
                appendLog(job->id, "INFO", "开始下载文件 " + file.remotePath + " (" + humanBytes(file.size) + ")");
                std::string code, message, etag, lastModified;
                const bool ok = downloadFile(job, file.id, file.remotePath, file.localPath, file.size, file.checksum,
                                             etag, lastModified, code, message);
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->active.erase(file.remotePath);
                }
                if (!ok) {
                    if (!job->cancelled.load()) {
                        db_.query("UPDATE download_files SET status='failed', error_message=?, updated_at=? WHERE id=?",
                                  {message, nowMs(), file.id});
                        appendLog(job->id, "ERROR", "文件下载失败 " + file.remotePath + ": " + message);
                        std::lock_guard<std::mutex> lock(job->mutex);
                        if (job->firstError.empty()) {
                            job->firstError = message;
                            job->firstErrorCode = code;
                        }
                    }
                    if (code == "AUTH_REQUIRED" || code == "NOT_FOUND") break;
                } else {
                    {
                        std::lock_guard<std::mutex> lock(job->filesMutex);
                        job->completedIds.insert(file.id);
                    }
                    appendLog(job->id, "INFO", "文件完成 " + file.remotePath);
                }
            }
        });
    }

    int64_t lastPersist = 0;
    int64_t lastSample = nowMs();
    uint64_t lastBytes = 0;
    bool pausedLogged = false;
    bool verifyingLogged = false;
    while (true) {
        bool anyActive = false;
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            anyActive = !job->active.empty();
        }
        if (activeWorkers.load() == 0) break;
        (void)anyActive;

        uint64_t downloaded = 0;
        uint64_t completed = 0;
        int completedFiles = 0;
        std::string current;
        {
            std::lock_guard<std::mutex> lock(job->filesMutex);
            for (const auto& file : job->files) {
                const fs::path finalPath = pathFromUtf8(file.localPath);
                const fs::path partPath = finalPath.u8string() + ".mfpart";
                if (file.status == "completed" || job->completedIds.count(file.id)) {
                    downloaded += file.size;
                    completed += file.size;
                    ++completedFiles;
                } else {
                    const int64_t partSize = fileSizeOrZero(partPath);
                    const uint64_t usable = file.size > 0
                        ? static_cast<uint64_t>(std::max<int64_t>(0, std::min<int64_t>(partSize, static_cast<int64_t>(file.size))))
                        : static_cast<uint64_t>(std::max<int64_t>(0, partSize));
                    downloaded += usable;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            for (const auto& name : job->active) {
                if (!current.empty()) current += ", ";
                current += name;
            }
        }
        const int64_t now = nowMs();
        const double seconds = static_cast<double>(now - lastSample) / 1000.0;
        if (seconds >= 0.4) {
            const double instant = static_cast<double>(downloaded >= lastBytes ? downloaded - lastBytes : 0) / seconds;
            job->speed = job->speed > 0 ? (job->speed * 0.6 + instant * 0.4) : instant;
            lastSample = now;
            lastBytes = downloaded;
        }
        job->downloadedBytes = downloaded;
        job->currentFiles = current;
        const int progress = job->totalBytes > 0
            ? static_cast<int>(std::min<uint64_t>(100, downloaded * 100 / job->totalBytes))
            : 0;

        const std::string state = job->cancelled.load() ? "cancelled"
                                 : (job->paused.load() ? "paused" : "downloading");
        if (job->paused.load()) {
            if (!pausedLogged) {
                persistTask(job, "paused", false);
                pausedLogged = true;
            }
        } else {
            pausedLogged = false;
            if (now - lastPersist > 1200) {
                updateTaskCounters(job, state, current);
                lastPersist = now;
            }
        }
        events_.publish(json{{"type", "download.progress"}, {"taskId", job->id}, {"status", state},
                             {"progress", progress}, {"downloadedBytes", downloaded}, {"totalBytes", job->totalBytes},
                             {"speed", job->speed}, {"completedFiles", completedFiles},
                             {"fileCount", static_cast<int>(job->files.size())}, {"currentFiles", current}});
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    // Recompute progress from persisted per-file state so the verifying stage shows real totals.
    {
        const auto doneRows = db_.query(
            "SELECT COALESCE(SUM(file_size),0) AS done, COALESCE(SUM(CASE WHEN status='completed' THEN 1 ELSE 0 END),0) AS files "
            "FROM download_files WHERE task_id=?", {job->id});
        if (!doneRows.empty()) {
            job->downloadedBytes = doneRows[0].value("done", 0ull);
            job->completedFiles = static_cast<int>(doneRows[0].value("files", 0ll));
        }
        job->speed = 0;
        job->currentFiles.clear();
    }

    if (job->cancelled.load()) {
        const auto sums = db_.query(
            "SELECT COALESCE(SUM(CASE WHEN status='completed' THEN file_size ELSE downloaded_size END),0) AS done "
            "FROM download_files WHERE task_id=?", {job->id});
        if (!sums.empty()) job->downloadedBytes = sums[0].value("done", 0ull);
        job->speed = 0;
        job->currentFiles.clear();
        updateTaskCounters(job, "cancelled", "");
        db_.query("UPDATE download_tasks SET status='cancelled', completed_at=?, updated_at=?, speed=0 WHERE id=?",
                  {nowMs(), nowMs(), job->id});
        appendLog(job->id, "WARNING", "任务已取消，已下载分片保留在 .mfpart 文件中");
        publishTaskEvent(job, "download.cancelled");
        return;
    }

    // Verify stage: per-file size and SHA256 checks already happened while downloading.
    updateTaskCounters(job, "verifying", "");
    db_.query("UPDATE download_tasks SET status='verifying', updated_at=? WHERE id=?", {nowMs(), job->id});
    if (!verifyingLogged) appendLog(job->id, "INFO", "校验已下载文件");
    const auto failedRows = db_.query("SELECT COUNT(*) AS n FROM download_files WHERE task_id=? AND status!='completed'",
                                      {job->id});
    const int failedCount = failedRows.empty() ? 0 : failedRows[0].value("n", 0);
    if (failedCount > 0) {
        const std::string message = job->firstError.empty()
            ? ("有 " + std::to_string(failedCount) + " 个文件未完成")
            : job->firstError;
        db_.query("UPDATE download_tasks SET status='failed', downloaded_bytes=(SELECT COALESCE(SUM(file_size),0) FROM "
                  "download_files WHERE task_id=? AND status='completed'), error_message=?, completed_at=?, updated_at=?, "
                  "speed=0, current_files='' WHERE id=?",
                  {job->id, message, nowMs(), nowMs(), job->id});
        appendLog(job->id, "ERROR", "下载失败: " + message);
        publishTaskEvent(job, "download.failed", json{{"error", message}, {"code", job->firstErrorCode}});
        return;
    }

    json importResult = json::object();
    try {
        if (job->autoImport) importResult = finalizeImport(job);
    } catch (const std::exception& ex) {
        importResult = json{{"kind", "error"}, {"message", ex.what()}};
        appendLog(job->id, "WARNING", std::string("下载完成但自动导入失败: ") + ex.what());
    }

    const uint64_t total = job->totalBytes;
    db_.query("UPDATE download_tasks SET status='completed', downloaded_bytes=?, total_bytes=?, progress=100, speed=0, "
              "completed_files=file_count, completed_at=?, updated_at=?, result_json=?, error_message='' WHERE id=?",
              {total, total, nowMs(), nowMs(), importResult.dump(), job->id});
    appendLog(job->id, "INFO", "下载完成，共 " + humanBytes(total) + "；" + importResult.value("summary", "未执行自动导入"));
    publishTaskEvent(job, "download.completed", json{{"result", importResult}, {"totalBytes", total}});
}

void DownloadManager::updateTaskCounters(const std::shared_ptr<Job>& job, const std::string& status,
                                         const std::string& currentFiles) {
    db_.query("UPDATE download_tasks SET status=?, total_bytes=?, downloaded_bytes=?, speed=?, current_files=?, updated_at=? WHERE id=?",
              {status, job->totalBytes, job->downloadedBytes, job->speed, currentFiles, nowMs(), job->id});
}

void DownloadManager::persistTask(const std::shared_ptr<Job>& job, const std::string& status, bool finished) {
    db_.query("UPDATE download_tasks SET status=?, downloaded_bytes=?, total_bytes=?, speed=?, updated_at=?, "
              "completed_at=CASE WHEN ? THEN ? ELSE completed_at END WHERE id=?",
              {status, job->downloadedBytes, job->totalBytes, job->speed, nowMs(), finished ? 1 : 0, nowMs(), job->id});
}

bool DownloadManager::downloadFile(const std::shared_ptr<Job>& job, int64_t fileId, const std::string& remotePath,
                                   const std::string& localPath, uint64_t expectedSize, const std::string& checksum,
                                   std::string& etag, std::string& lastModified, std::string& errorCode,
                                   std::string& error) {
    const fs::path finalPath = pathFromUtf8(localPath);
    const fs::path partPath = finalPath.u8string() + ".mfpart";
    std::error_code ec;
    fs::create_directories(finalPath.parent_path(), ec);
    if (ec) {
        errorCode = "LOCAL_IO_ERROR";
        error = "无法创建目录: " + pathToUtf8(finalPath.parent_path()) + " (" + ec.message() + ")";
        return false;
    }

    if (expectedSize > 0 && fs::exists(finalPath, ec) && static_cast<uint64_t>(fs::file_size(finalPath, ec)) == expectedSize) {
        if (checksum.empty()) {
            db_.query("UPDATE download_files SET status='completed', downloaded_size=?, updated_at=? WHERE id=?",
                      {expectedSize, nowMs(), fileId});
            appendLog(job->id, "INFO", "本地文件已存在且大小一致，跳过下载: " + remotePath);
            return true;
        }
        const std::string digest = sha256File(finalPath);
        if (digest == checksum) {
            db_.query("UPDATE download_files SET status='completed', downloaded_size=?, sha256=?, updated_at=? WHERE id=?",
                      {expectedSize, digest, nowMs(), fileId});
            appendLog(job->id, "INFO", "本地文件已存在且 SHA256 一致，跳过下载: " + remotePath);
            return true;
        }
        fs::remove(finalPath, ec);
        appendLog(job->id, "WARNING", "本地已存在文件哈希不匹配，重新下载: " + remotePath);
    }

    uint64_t offset = 0;
    if (fs::exists(partPath, ec)) {
        offset = static_cast<uint64_t>(std::max<int64_t>(0, fileSizeOrZero(partPath)));
    }
    if (expectedSize > 0 && offset > expectedSize) {
        fs::remove(partPath, ec);
        offset = 0;
    }
    if (expectedSize > 0 && offset == expectedSize) {
        // Part file already complete (e.g. crash after body write).
        const std::string digest = checksum.empty() ? std::string() : sha256File(partPath);
        if (checksum.empty() || digest == checksum) {
            fs::remove(finalPath, ec);
            fs::rename(partPath, finalPath, ec);
            if (!ec) {
                db_.query("UPDATE download_files SET status='completed', downloaded_size=?, sha256=?, updated_at=? WHERE id=?",
                          {expectedSize, checksum, nowMs(), fileId});
                return true;
            }
        }
        fs::remove(partPath, ec);
        offset = 0;
    }

    std::string lastError = "下载失败";
    std::string lastCode = "DOWNLOAD_FAILED";
    for (int attempt = 0; attempt < kMaxFileRetries; ++attempt) {
        if (job->cancelled.load()) {
            errorCode = "CANCELLED";
            error = "任务已取消";
            return false;
        }
        waitWhilePaused(job);
        if (job->cancelled.load()) {
            errorCode = "CANCELLED";
            error = "任务已取消";
            return false;
        }
        job->abort.store(false);

        HttpDownloadOptions options;
        options.url = HfClient::resolveUrl(job->repoId, job->revision, remotePath);
        options.targetPath = partPath;
        options.resumeOffset = offset;
        options.connectTimeoutMs = 20000;
        options.receiveTimeoutMs = 60000;
        options.limiter = job->limiter.get();
        options.abortFlag = &job->abort;
        if (!job->token.empty()) options.headers.push_back({"Authorization", "Bearer " + job->token});
        if (offset > 0 && !etag.empty()) options.headers.push_back({"If-Range", etag});

        HttpDownloadResult result = HttpClient::download(options);
        etag = result.etag.empty() ? etag : result.etag;
        lastModified = result.lastModified.empty() ? lastModified : result.lastModified;
        if (result.ok) {
            const int64_t partSize = fileSizeOrZero(partPath);
            if (expectedSize > 0 && static_cast<uint64_t>(partSize) != expectedSize) {
                lastCode = "SIZE_MISMATCH";
                lastError = "文件大小不一致：期望 " + humanBytes(expectedSize) + "，实际 " + humanBytes(static_cast<uint64_t>(partSize));
                appendLog(job->id, "WARNING", lastError + "，将重新下载 " + remotePath);
                fs::remove(partPath, ec);
                offset = 0;
                continue;
            }
            if (!checksum.empty() && job->verifyChecksum) {
                const std::string digest = sha256File(partPath);
                if (digest != checksum) {
                    lastCode = "CHECKSUM_MISMATCH";
                    lastError = "SHA256 校验失败（期望 " + checksum.substr(0, 16) + "…，实际 " + digest.substr(0, 16) + "…）";
                    appendLog(job->id, "WARNING", lastError + "，删除损坏分片并重试 " + remotePath);
                    fs::remove(partPath, ec);
                    offset = 0;
                    continue;
                }
            }
            fs::remove(finalPath, ec);
            fs::rename(partPath, finalPath, ec);
            if (ec) {
                errorCode = "LOCAL_IO_ERROR";
                error = "无法重命名临时文件: " + ec.message();
                return false;
            }
            db_.query("UPDATE download_files SET status='completed', local_path=?, downloaded_size=?, etag=?, last_modified=?, "
                      "sha256=?, error_message='', updated_at=? WHERE id=?",
                      {pathToUtf8(finalPath), expectedSize, etag, lastModified, checksum, nowMs(), fileId});
            return true;
        }

        offset = static_cast<uint64_t>(std::max<int64_t>(0, fileSizeOrZero(partPath)));
        if (expectedSize > 0 && offset > expectedSize) {
            fs::remove(partPath, ec);
            offset = 0;
        }
        db_.query("UPDATE download_files SET downloaded_size=?, etag=?, last_modified=?, retry_count=retry_count+1, updated_at=? WHERE id=?",
                  {offset, etag, lastModified, nowMs(), fileId});

        if (result.authFailure()) {
            errorCode = "AUTH_REQUIRED";
            error = result.status == 401 ? "需要有效的 Hugging Face Token（HTTP 401）"
                                         : "没有下载权限或未接受模型许可协议（HTTP 403）";
            return false;
        }
        if (result.status == 404) {
            errorCode = "NOT_FOUND";
            error = "远端文件不存在（HTTP 404）: " + remotePath;
            return false;
        }
        lastCode = result.status == 429 ? "RATE_LIMITED" : (result.status >= 500 ? "SERVER_ERROR" : "NETWORK_ERROR");
        lastError = HttpClient::redact(result.error.empty() ? ("HTTP " + std::to_string(result.status)) : result.error);
        if (job->cancelled.load()) {
            errorCode = "CANCELLED";
            error = "任务已取消";
            return false;
        }
        if (job->paused.load()) {
            // Pause is not a failure: keep the file pending and retry after resume.
            --attempt;
            continue;
        }
        if (!result.retryable() && result.status != 0) {
            errorCode = lastCode;
            error = lastError;
            return false;
        }
        const int backoff = std::min(8000, 500 * (1 << attempt));
        appendLog(job->id, "WARNING", "下载失败（" + lastError + "），" + std::to_string(backoff / 1000.0) + " 秒后重试 " +
                  remotePath + "（" + std::to_string(attempt + 1) + "/" + std::to_string(kMaxFileRetries) + "）");
        db_.query("UPDATE download_tasks SET status='retrying', updated_at=? WHERE id=?", {nowMs(), job->id});
        if (!waitInterruptible(job, backoff)) {
            errorCode = "CANCELLED";
            error = "任务已取消";
            return false;
        }
    }
    errorCode = lastCode;
    error = lastError + "（已重试 " + std::to_string(kMaxFileRetries) + " 次）";
    return false;
}

json DownloadManager::finalizeImport(const std::shared_ptr<Job>& job) {
    const fs::path dir = pathFromUtf8(job->destination);
    std::error_code ec;
    std::vector<fs::path> ggufFiles;
    bool hasSafetensors = false;
    bool hasConfig = false;
    uint64_t ggufSize = 0;
    fs::path largestGguf;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = toLower(pathToUtf8(it->path().extension()));
        if (ext == ".gguf") {
            ggufFiles.push_back(it->path());
            const int64_t size = fileSizeOrZero(it->path());
            if (static_cast<uint64_t>(size) > ggufSize) {
                ggufSize = static_cast<uint64_t>(size);
                largestGguf = it->path();
            }
        } else if (ext == ".safetensors") {
            hasSafetensors = true;
        } else if (toLower(pathToUtf8(it->path().filename())) == "config.json") {
            hasConfig = true;
        }
    }

    json result;
    if (!ggufFiles.empty()) {
        const GgufInfo info = GgufReader::read(largestGguf, true);
        result = json{{"kind", "gguf"},
                      {"ggufPath", pathToUtf8(largestGguf)},
                      {"ggufCount", static_cast<int>(ggufFiles.size())},
                      {"verification", info.toJson()},
                      {"summary", "识别为 GGUF 仓库，已通过 GGUF 结构校验（" + std::to_string(ggufFiles.size()) + " 个文件）"}};
        const int64_t ggufVersionId = 0;
        (void)ggufVersionId;
        return result;
    }
    if (hasConfig && hasSafetensors) {
        std::string code, error;
        json model = system_.importModel(pathToUtf8(dir), code, error);
        if (error.empty() && model.is_object()) {
            result = json{{"kind", "safetensors"},
                          {"modelId", model.value("id", "")},
                          {"name", model.value("name", "")},
                          {"architecture", model.value("architecture", "")},
                          {"parameterCount", model.value("parameterCount", 0ull)},
                          {"supported", model.value("supported", false)},
                          {"summary", "已导入模型库：" + model.value("name", "")}};
            db_.query("INSERT INTO conversion_history(id,model_id,task_id,event,detail,created_at) VALUES(?,?,?,?,?,?)",
                      {newId("hist"), model.value("id", ""), job->id, "MODEL_DOWNLOADED",
                       "从 Hugging Face 下载并导入: " + job->repoId, isoNow()});
        } else {
            result = json{{"kind", "safetensors"}, {"importError", error}, {"code", code},
                          {"summary", "下载完成，但自动导入失败: " + error}};
        }
        return result;
    }
    result = json{{"kind", "unknown"},
                  {"summary", "下载完成，但目录中未发现完整的 config.json + safetensors 或 GGUF 文件"},
                  {"hasConfig", hasConfig}, {"hasSafetensors", hasSafetensors},
                  {"ggufCount", static_cast<int>(ggufFiles.size())}};
    return result;
}

void DownloadManager::appendLog(const std::string& taskId, const std::string& level, const std::string& message) {
    int64_t seq = 0;
    const auto rows = db_.query("SELECT COALESCE(MAX(seq),0) AS seq FROM download_logs WHERE task_id=?", {taskId});
    if (!rows.empty()) seq = rows[0].value("seq", 0ll) + 1;
    const std::string safe = HttpClient::redact(sanitizeSingleLine(message, 2000));
    db_.query("INSERT INTO download_logs(task_id,seq,level,message,created_at) VALUES(?,?,?,?,?)",
              {taskId, seq, level, safe, isoNow()});
    events_.publish(json{{"type", "download.log"}, {"taskId", taskId}, {"seq", seq}, {"level", level},
                         {"message", safe}, {"time", isoNow()}});
}

void DownloadManager::publishTaskEvent(const std::shared_ptr<Job>& job, const std::string& type, const json& extra) {
    json event{{"type", type}, {"taskId", job->id}, {"repoId", job->repoId},
               {"downloadedBytes", job->downloadedBytes}, {"totalBytes", job->totalBytes}};
    for (auto it = extra.begin(); it != extra.end(); ++it) event[it.key()] = it.value();
    events_.publish(event);
}

bool DownloadManager::createTask(const DownloadRequest& request, std::string& taskId, std::string& code,
                                 std::string& error) {
    if (request.repoId.empty()) {
        code = "INVALID_REPOSITORY";
        error = "缺少仓库 ID";
        return false;
    }
    if (request.files.empty()) {
        code = "NO_FILES_SELECTED";
        error = "没有选择任何文件";
        return false;
    }
    if (request.destination.empty()) {
        code = "INVALID_DESTINATION";
        error = "缺少本地保存目录";
        return false;
    }
    const fs::path destination = fs::absolute(pathFromUtf8(request.destination));
    if (!ensureDirectory(destination, error)) {
        code = "INVALID_DESTINATION";
        return false;
    }
    if (!directoryWritable(destination)) {
        code = "DESTINATION_NOT_WRITABLE";
        error = "保存目录不可写: " + pathToUtf8(destination);
        return false;
    }

    // Reject duplicates and unsafe paths before touching the database.
    std::set<std::string> unique;
    uint64_t totalBytes = 0;
    std::vector<std::pair<std::string, fs::path>> prepared;
    for (const auto& remote : request.files) {
        if (!unique.insert(remote).second) continue;
        std::string pathError;
        const fs::path local = HfClient::localPathFor(destination, remote, pathError);
        if (local.empty()) {
            code = "UNSAFE_PATH";
            error = pathError;
            return false;
        }
        prepared.push_back({remote, local});
    }
    if (prepared.empty()) {
        code = "NO_FILES_SELECTED";
        error = "过滤后没有有效文件";
        return false;
    }

    // Reuse the repository row when the same commit was resolved before.
    std::string repositoryId;
    const auto existing = db_.query("SELECT id,total_size,file_count FROM hf_repositories WHERE repo_id=? AND revision=? AND commit_hash=?",
                                    {request.repoId, request.revision, request.commitHash});
    if (!existing.empty()) {
        repositoryId = existing[0].value("id", "");
    } else {
        repositoryId = newId("repo");
        db_.query("INSERT INTO hf_repositories(id,repo_id,revision,commit_hash,repo_type,total_size,file_count,license,gated,raw_json,created_at,updated_at) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                  {repositoryId, request.repoId, request.revision, request.commitHash, "model", 0, 0, "", 0,
                   request.selection.dump(), isoNow(), isoNow()});
    }

    json repoMeta = request.selection.contains("repository") && request.selection["repository"].is_object()
        ? request.selection["repository"] : json::object();
    if (!repoMeta.empty()) {
        db_.query("UPDATE hf_repositories SET license=?, gated=?, raw_json=?, updated_at=? WHERE id=?",
                  {repoMeta.value("license", ""), repoMeta.value("gated", false) ? 1 : 0, repoMeta.dump(), isoNow(), repositoryId});
    }

    totalBytes = 0;
    const std::string id = newId("dl");
    const int concurrency = std::max(1, std::min(request.concurrency <= 0 ? 4 : request.concurrency, kMaxConcurrency));
    db_.query("INSERT INTO download_tasks(id,repository_id,repo_id,revision,commit_hash,destination,status,total_bytes,"
              "downloaded_bytes,speed,concurrency,max_speed,retry_count,file_count,completed_files,current_files,"
              "selection_json,error_message,result_json,created_at,updated_at,completed_at) "
              "VALUES(?,?,?,?,?,?,'pending',?,0,0,?,?,0,?,0,'',?,'','',?,?,0)",
              {id, repositoryId, request.repoId, request.revision, request.commitHash, pathToUtf8(destination),
               totalBytes, concurrency, request.maxSpeed, static_cast<int>(prepared.size()),
               request.selection.dump(), nowMs(), nowMs()});

    // File sizes come from the resolve step; the API passes them through the selection payload.
    json sizes = request.selection.contains("sizes") ? request.selection["sizes"] : json::object();
    for (const auto& item : prepared) {
        uint64_t size = 0;
        std::string checksum;
        if (sizes.is_object() && sizes.contains(item.first)) {
            const auto& entry = sizes[item.first];
            if (entry.is_object()) {
                size = entry.value("size", 0ull);
                checksum = entry.value("sha256", "");
            } else if (entry.is_number()) {
                size = entry.get<uint64_t>();
            }
        }
        if (size > 0) totalBytes += size;
        db_.query("INSERT INTO download_files(task_id,remote_path,local_path,file_size,downloaded_size,etag,last_modified,"
                  "checksum,status,retry_count,error_message,updated_at) VALUES(?,?,?,?,0,'','',?,'pending',0,'',?)",
                  {id, item.first, pathToUtf8(item.second), size, checksum, nowMs()});
    }
    db_.query("UPDATE download_tasks SET total_bytes=? WHERE id=?", {totalBytes, id});
    db_.query("UPDATE hf_repositories SET total_size=?, file_count=?, updated_at=? WHERE id=?",
              {totalBytes, static_cast<int>(prepared.size()), isoNow(), repositoryId});
    appendLog(id, "INFO", "任务创建：选择了 " + std::to_string(prepared.size()) + " 个文件，共 " + humanBytes(totalBytes));
    events_.publish(json{{"type", "download.created"}, {"taskId", id}, {"repoId", request.repoId},
                         {"fileCount", prepared.size()}, {"totalBytes", totalBytes}});
    taskId = id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    code.clear();
    error.clear();
    return true;
}

json DownloadManager::loadTaskJson(const std::string& id, bool includeFiles) {
    const auto rows = db_.query("SELECT * FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) return json::object();
    const json row = rows[0];
    const uint64_t total = row.value("total_bytes", 0ull);
    const uint64_t downloaded = row.value("downloaded_bytes", 0ull);
    const std::string status = row.value("status", "");
    json result;
    try {
        result = row.value("result_json", "").empty() ? json::object() : json::parse(row.value("result_json", "{}"));
    } catch (...) {
        result = json::object();
    }
    int progress = total > 0 ? static_cast<int>(std::min<uint64_t>(100, downloaded * 100 / total)) : 0;
    if (status == "completed") progress = 100;
    json task{
        {"id", id},
        {"repoId", row.value("repo_id", "")},
        {"revision", row.value("revision", "")},
        {"commitHash", row.value("commit_hash", "")},
        {"destination", row.value("destination", "")},
        {"status", status},
        {"statusLabel", statusLabel(status)},
        {"progress", progress},
        {"totalBytes", total},
        {"downloadedBytes", status == "completed" ? total : downloaded},
        {"speed", row.value("speed", 0.0)},
        {"concurrency", row.value("concurrency", 4)},
        {"maxSpeed", row.value("max_speed", 0ull)},
        {"fileCount", row.value("file_count", 0)},
        {"completedFiles", row.value("completed_files", 0)},
        {"currentFiles", row.value("current_files", "")},
        {"retryCount", row.value("retry_count", 0)},
        {"errorMessage", row.value("error_message", "")},
        {"createdAt", row.value("created_at", 0ll)},
        {"updatedAt", row.value("updated_at", 0ll)},
        {"completedAt", row.value("completed_at", 0ll)},
        {"result", result}
    };
    // Live values from the in-memory job take precedence over the throttled DB row.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it != jobs_.end()) {
            const auto& job = it->second;
            if (job->totalBytes > 0) task["totalBytes"] = job->totalBytes;
            task["downloadedBytes"] = job->downloadedBytes;
            task["speed"] = job->speed;
            task["currentFiles"] = job->currentFiles;
            if (job->totalBytes > 0) {
                task["progress"] = static_cast<int>(std::min<uint64_t>(100, job->downloadedBytes * 100 / job->totalBytes));
            }
        }
    }
    if (includeFiles) task["files"] = files(id);
    return task;
}

json DownloadManager::list(int limit) {
    limit = std::max(1, std::min(limit, 200));
    json items = json::array();
    const auto rows = db_.query("SELECT id FROM download_tasks ORDER BY created_at DESC LIMIT ?", {limit});
    for (const auto& row : rows) {
        json task = loadTaskJson(row.value("id", ""), false);
        if (!task.empty()) items.push_back(task);
    }
    return json{{"items", items}, {"settings", settings()}, {"engineRunning", ownsLock_.load()}};
}

json DownloadManager::getTask(const std::string& id, bool includeFiles) {
    json task = loadTaskJson(id, includeFiles);
    if (task.empty()) return json::object();
    const auto repoRows = db_.query("SELECT * FROM hf_repositories WHERE id=(SELECT repository_id FROM download_tasks WHERE id=?)",
                                    {id});
    if (!repoRows.empty()) {
        const json& repo = repoRows[0];
        task["repository"] = json{{"repoId", repo.value("repo_id", "")}, {"revision", repo.value("revision", "")},
                                  {"commitHash", repo.value("commit_hash", "")}, {"license", repo.value("license", "")},
                                  {"gated", repo.value("gated", 0) != 0}};
    }
    return task;
}

json DownloadManager::files(const std::string& id) {
    json items = json::array();
    const auto rows = db_.query("SELECT * FROM download_files WHERE task_id=? ORDER BY remote_path", {id});
    for (const auto& row : rows) {
        items.push_back(json{
            {"id", row.value("id", 0ll)}, {"remotePath", row.value("remote_path", "")},
            {"localPath", row.value("local_path", "")}, {"size", row.value("file_size", 0ull)},
            {"downloaded", row.value("downloaded_size", 0ull)}, {"status", row.value("status", "")},
            {"etag", row.value("etag", "")}, {"checksum", row.value("checksum", "")},
            {"retryCount", row.value("retry_count", 0)}, {"error", row.value("error_message", "")},
            {"updatedAt", row.value("updated_at", 0ll)}
        });
    }
    return items;
}

json DownloadManager::logs(const std::string& id, int64_t afterSeq, int limit) {
    limit = std::max(1, std::min(limit, 1000));
    json items = json::array();
    const auto rows = db_.query("SELECT * FROM download_logs WHERE task_id=? AND seq>? ORDER BY seq ASC LIMIT ?",
                                {id, afterSeq, limit});
    for (const auto& row : rows) {
        items.push_back(json{{"seq", row.value("seq", 0ll)}, {"level", row.value("level", "INFO")},
                             {"message", row.value("message", "")}, {"time", row.value("created_at", "")}});
    }
    return json{{"items", items}, {"lastSeq", items.empty() ? afterSeq : items.back().value("seq", afterSeq)}};
}

bool DownloadManager::pause(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) {
        error = "下载任务不存在";
        return false;
    }
    const std::string status = rows[0].value("status", "");
    if (status == "completed" || status == "failed" || status == "cancelled") {
        error = "任务已结束，无法暂停";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it != jobs_.end()) {
            it->second->paused.store(true);
            it->second->abort.store(true);
            it->second->cv.notify_all();
        }
    }
    db_.query("UPDATE download_tasks SET status='paused', speed=0, updated_at=?, error_message='' WHERE id=?",
              {nowMs(), id});
    appendLog(id, "INFO", "任务已暂停，已下载分片保留");
    events_.publish(json{{"type", "download.paused"}, {"taskId", id}});
    return true;
}

bool DownloadManager::resume(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) {
        error = "下载任务不存在";
        return false;
    }
    const std::string status = rows[0].value("status", "");
    if (status == "completed" || status == "cancelled") {
        error = "该任务已结束，不能继续";
        return false;
    }
    if (status == "failed") {
        return retry(id, error);
    }
    db_.query("UPDATE download_tasks SET status='downloading', error_message='', updated_at=? WHERE id=?", {nowMs(), id});
    appendLog(id, "INFO", "继续下载，从断点恢复");
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it != jobs_.end()) {
            known = true;
            it->second->paused.store(false);
            it->second->cancelled.store(false);
            it->second->abort.store(false);
            it->second->cv.notify_all();
        }
        cv_.notify_all();
    }
    if (!known) {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    events_.publish(json{{"type", "download.resumed"}, {"taskId", id}});
    return true;
}

bool DownloadManager::cancel(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) {
        error = "下载任务不存在";
        return false;
    }
    const std::string status = rows[0].value("status", "");
    if (status == "completed") {
        error = "任务已完成，无需取消";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it != jobs_.end()) {
            it->second->cancelled.store(true);
            it->second->paused.store(false);
            it->second->abort.store(true);
            it->second->cv.notify_all();
        }
    }
    db_.query("UPDATE download_tasks SET status='cancelled', speed=0, completed_at=?, updated_at=? WHERE id=?",
              {nowMs(), nowMs(), id});
    appendLog(id, "WARNING", "用户取消任务");
    events_.publish(json{{"type", "download.cancelled"}, {"taskId", id}});
    return true;
}

bool DownloadManager::retry(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) {
        error = "下载任务不存在";
        return false;
    }
    const std::string status = rows[0].value("status", "");
    if (status == "downloading" || status == "retrying") {
        error = "任务正在运行";
        return false;
    }
    db_.query("UPDATE download_files SET status='pending', error_message='', retry_count=0 WHERE task_id=? AND status!='completed'",
              {id});
    db_.query("UPDATE download_tasks SET status='pending', error_message='', completed_at=0, updated_at=?, speed=0 WHERE id=?",
              {nowMs(), id});
    appendLog(id, "INFO", "重新开始下载失败或不完整的文件");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    events_.publish(json{{"type", "download.retry"}, {"taskId", id}});
    return true;
}

bool DownloadManager::remove(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT status FROM download_tasks WHERE id=?", {id});
    if (rows.empty()) {
        error = "下载任务不存在";
        return false;
    }
    const std::string status = rows[0].value("status", "");
    if (status == "downloading" || status == "retrying" || status == "verifying") {
        error = "请先取消正在运行的任务再删除";
        return false;
    }
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it != jobs_.end()) {
            job = it->second;
            jobs_.erase(it);
        }
    }
    if (job && job->runner.joinable()) {
        // The task is already finished from the database point of view; join when it has stopped,
        // otherwise detach so the thread's shared_ptr keeps the job alive until it exits.
        if (job->finished.load()) job->runner.join();
        else job->runner.detach();
    }
    db_.query("DELETE FROM download_logs WHERE task_id=?", {id});
    db_.query("DELETE FROM download_files WHERE task_id=?", {id});
    db_.query("DELETE FROM download_tasks WHERE id=?", {id});
    return true;
}

void DownloadManager::stop() {
    if (stop_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : jobs_) {
            item.second->paused.store(false);
            item.second->abort.store(true);
            item.second->cv.notify_all();
        }
        cv_.notify_all();
    }
    if (loop_.joinable()) loop_.join();
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : jobs_) jobs.push_back(item.second);
        jobs_.clear();
    }
    for (auto& job : jobs) {
        if (job->runner.joinable()) job->runner.join();
        if (!job->cancelled.load()) {
            db_.query("UPDATE download_tasks SET status='paused', speed=0, updated_at=?, "
                      "error_message='应用退出，下载断点已保留' WHERE id=? AND status IN ('downloading','retrying','verifying')",
                      {nowMs(), job->id});
        }
    }
    releaseLock();
}

} // namespace mf
