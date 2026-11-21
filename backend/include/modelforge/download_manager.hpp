#pragma once

#include "modelforge/database.hpp"
#include "modelforge/event_hub.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/system_service.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mf {

struct DownloadRequest {
    std::string repoId;
    std::string revision = "main";
    std::string commitHash;
    std::string destination;
    std::vector<std::string> files;
    int concurrency = 4;
    uint64_t maxSpeed = 0;
    bool verifyChecksum = true;
    bool autoImport = true;
    json selection = json::object();
};

class DownloadManager {
public:
    DownloadManager(Database& db, SettingsService& settings, SystemService& system, EventHub& events);
    ~DownloadManager();

    void start();
    void stop();

    bool createTask(const DownloadRequest& request, std::string& taskId, std::string& code, std::string& error);
    json list(int limit);
    json getTask(const std::string& id, bool includeFiles = false);
    json files(const std::string& id);
    json logs(const std::string& id, int64_t afterSeq, int limit);
    bool pause(const std::string& id, std::string& error);
    bool resume(const std::string& id, std::string& error);
    bool cancel(const std::string& id, std::string& error);
    bool retry(const std::string& id, std::string& error);
    bool remove(const std::string& id, std::string& error);

    json settings();
    bool updateSettings(const json& patch, std::string& error);
    bool hasToken() const;
    std::string token() const;
    void setToken(const std::string& token, std::string& error);
    json tokenStatus();

    bool ownsScheduler() const { return ownsLock_; }

    struct Job;
    using JobPtr = std::shared_ptr<Job>;

private:

    Database& db_;
    SettingsService& settings_;
    SystemService& system_;
    EventHub& events_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs_;
    std::thread loop_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ownsLock_{false};
#ifdef _WIN32
    void* lockHandle_ = nullptr;
#else
    int lockFd_ = -1;
#endif

    bool tryAcquireLock();
    void releaseLock();
    void schedulerLoop();
    void startJob(const std::string& id);
    void runJob(std::shared_ptr<Job> job);
    bool downloadFile(const std::shared_ptr<Job>& job, int64_t fileId, const std::string& remotePath,
                      const std::string& localPath, uint64_t expectedSize, const std::string& checksum,
                      std::string& etag, std::string& lastModified, std::string& errorCode, std::string& error);
    void pullFilesIntoJob(const std::shared_ptr<Job>& job, std::string& error);
    void persistTask(const std::shared_ptr<Job>& job, const std::string& status, bool finished);
    void updateTaskCounters(const std::shared_ptr<Job>& job, const std::string& status, const std::string& currentFiles);
    void appendLog(const std::string& taskId, const std::string& level, const std::string& message);
    void publishTaskEvent(const std::shared_ptr<Job>& job, const std::string& type, const json& extra = json::object());
    json taskRow(const std::string& id);
    json loadTaskJson(const std::string& id, bool includeFiles);
    void recoverInterruptedTasks();
    json finalizeImport(const std::shared_ptr<Job>& job);
    std::string loadSetting(const std::string& key, const std::string& fallback) const;
    void saveSetting(const std::string& key, const std::string& value);
    std::string protectSecret(const std::string& plain, bool& ok);
    std::string unprotectSecret(const std::string& stored, bool& ok) const;
};

} // namespace mf
