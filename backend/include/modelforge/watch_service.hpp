#pragma once

#include "modelforge/database.hpp"
#include "modelforge/event_hub.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/system_service.hpp"
#include "modelforge/task_manager.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace mf {

// Watches a list of folders for new model directories. Polling is used on purpose:
// it behaves the same on every platform, survives network shares where change
// notifications are unreliable, and costs almost nothing at a 30 s cadence.
class WatchService {
public:
    WatchService(Database& db, SettingsService& settings, SystemService& system, TaskManager& tasks, EventHub& events);
    ~WatchService();

    void start();
    void stop();

    json status();
    json events(int limit);
    // force=true re-imports folders even when their signature did not change.
    json scan(bool force, std::string& error);
    // Wakes the loop so a settings change (enable, interval) takes effect quickly.
    void notify();

private:
    Database& db_;
    SettingsService& settings_;
    SystemService& system_;
    TaskManager& tasks_;
    EventHub& events_;

    std::thread loop_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> wake_{false};
    std::mutex scanMutex_;
    std::mutex stateMutex_;
    int64_t lastScanAt_ = 0;
    int64_t lastScanDurationMs_ = 0;
    std::string lastError_;

    void loopMain();
    json scanFolders(bool force);
    bool handleCandidate(const fs::path& dir, const std::string& folder, bool force, json& summary);
    std::string signatureFor(const fs::path& dir) const;
    json seenRow(const std::string& path) const;
    void remember(const std::string& path, const std::string& signature, const std::string& modelId,
                  const std::string& taskId);
    void recordEvent(const std::string& folder, const std::string& path, const std::string& action,
                     const std::string& modelId, const std::string& taskId, const std::string& detail);
};

} // namespace mf
