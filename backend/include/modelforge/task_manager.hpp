#pragma once

#include "modelforge/converter.hpp"
#include "modelforge/database.hpp"
#include "modelforge/event_hub.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/types.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace mf {

class TaskManager {
public:
    TaskManager(Database& db, SettingsService& settings, ConverterService& converter, EventHub& events);
    ~TaskManager();

    void start();
    void stop();

    bool createTask(ConversionParams params, std::string& taskId, std::string& code, std::string& error);
    json listTasks(const std::string& status = "", const std::string& modelId = "", int limit = 100);
    json getTask(const std::string& id);
    bool cancelTask(const std::string& id, std::string& error);
    bool pauseTask(const std::string& id, std::string& error);
    bool resumeTask(const std::string& id, std::string& error);
    bool removeTask(const std::string& id, bool deleteLogs, std::string& error);
    bool setPriority(const std::string& id, int priority, std::string& error);
    bool retryTask(const std::string& id, std::string& newId, std::string& error);
    json logs(const std::string& id, int afterSeq, int limit, const std::string& level);
    fs::path logFilePath(const std::string& id) const;
    json result(const std::string& id);
    // Per-stage process counters (CPU time, peak memory, disk read/write).
    json resources(const std::string& id);
    json queueStats();
    void setConcurrency(int value);
    std::string errorSuggestion(const std::string& code) const;

private:
    struct Runtime {
        TaskRecord task;
        std::atomic<bool> cancel{false};
        std::atomic<bool> cancelRequested{false};
        int64_t logSeq = 0;
        std::string stageDetail;
        uint64_t processedBytes = 0;
        uint64_t totalBytes = 0;
        int progress = 0;
        std::string progressMode = "estimated";
        std::string currentLogLevel;
        int64_t lastPersistAt = 0;
    };

    Database& db_;
    SettingsService& settings_;
    ConverterService& converter_;
    EventHub& events_;
    mutable std::mutex mutex_;
    std::mutex logMutex_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Runtime>> runtime_;
    std::map<std::string, std::thread> workers_;
    std::thread scheduler_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> schedulerOwned_{false};
    bool reconciled_ = false;
    int running_ = 0;
#ifdef _WIN32
    void* schedulerLockHandle_ = nullptr;
#else
    int schedulerLockFd_ = -1;
#endif
    bool tryAcquireSchedulerLock();
    void releaseSchedulerLock();

    void schedulerLoop();
    void runTask(const std::string& id);
    void finishTask(const std::shared_ptr<Runtime>& runtime, const std::string& status,
                    const std::string& stage, const std::string& code, const std::string& message);
    void updateStatus(const std::shared_ptr<Runtime>& rt, const std::string& status, const std::string& stage,
                      const std::string& detail, const std::string& code = "", const std::string& message = "");
    void updateProgress(const std::shared_ptr<Runtime>& rt, int progress, const std::string& mode,
                        const std::string& detail, uint64_t processed = 0, uint64_t total = 0);
    void appendLog(const std::shared_ptr<Runtime>& rt, const std::string& level, const std::string& message);
    std::string classifyError(const std::string& text, int exitCode, const std::string& stage) const;
    std::shared_ptr<Runtime> findRuntime(const std::string& id) const;
    void persistTask(const TaskRecord& task);
    TaskRecord taskFromRow(const json& row) const;
    void ensureDirs();
    std::string ensureModelRecord(const ModelScanResult& scan);
    uint64_t estimatedOutputSize(const ModelScanResult& scan, const std::string& type);
};

} // namespace mf
