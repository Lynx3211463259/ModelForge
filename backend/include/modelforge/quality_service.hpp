#pragma once

#include "modelforge/database.hpp"
#include "modelforge/event_hub.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/system_service.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mf {

// Perplexity evaluation uses the official llama-perplexity binary from the same
// release ModelForge ships. Nothing is simulated: a real process is spawned, and the
// reported PPL is whatever that process prints.
class QualityService {
public:
    QualityService(Database& db, SettingsService& settings, SystemService& system, EventHub& events);
    ~QualityService();

    void stop();

    // Starts an evaluation job. Accepted keys: modelId, targets[{path,versionId,quantType}],
    // context, threads, datasetPath.
    bool run(const json& options, std::string& error);
    bool cancel(std::string& error);

    json status() const;
    json results(int limit);
    bool remove(const std::string& id, std::string& error);
    json comparison(const std::string& modelId);
    json dataset() const;

private:
    struct Job {
        std::string status = "idle"; // idle|running|completed|failed|cancelled
        int progress = 0;
        int total = 0;
        int completed = 0;
        std::string currentTarget;
        std::string message;
        std::string errorCode;
        std::string errorMessage;
        int64_t startedAt = 0;
        int64_t finishedAt = 0;
        json results = json::array();
        std::vector<json> logs;
    };

    Database& db_;
    SettingsService& settings_;
    SystemService& system_;
    EventHub& events_;

    mutable std::mutex mutex_;
    Job job_;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    std::thread worker_;
    bool running_ = false;

    void workerMain(std::vector<json> targets, std::string datasetPath, std::string datasetKind,
                    int context, int threads, std::string modelId);
    void appendLog(const std::string& level, const std::string& message);
    bool ensureDataset(std::string& path, std::string& kind, std::string& error);
    json evaluateOne(const json& target, const std::string& datasetPath, const std::string& datasetKind,
                     int context, int threads, const std::string& modelId, const std::shared_ptr<std::atomic<bool>>& cancel,
                     std::string& errorCode, std::string& errorMessage);
    std::string versionIdForPath(const std::string& path) const;
    std::string modelIdForPath(const std::string& path) const;
};

} // namespace mf
