#pragma once

#include "modelforge/converter.hpp"
#include "modelforge/database.hpp"
#include "modelforge/model_deleter.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/task_manager.hpp"

#include <mutex>

namespace mf {

class SystemService {
public:
    SystemService(Database& db, SettingsService& settings, ConverterService& converter, TaskManager& tasks);

    json summary();
    json resources();
    json listModels(const std::string& query, bool favoritesOnly, int limit);
    json getModel(const std::string& id);
    // Version tree: the source model as root plus every GGUF produced for it.
    json modelVersions(const std::string& id);
    json importModel(const std::string& path, std::string& code, std::string& error);
    json reanalyzeModel(const std::string& id, std::string& error);
    bool updateModel(const std::string& id, const json& patch, std::string& error);
    // Deletes the record and, when deleteFiles is true, the files registered
    // for it. Returns a structured result so the API can explain partial
    // outcomes (missing/shared files, permission failures).
    ModelDeleteResult deleteModel(const std::string& id, bool deleteFiles);
    json history(int limit);
    json quantizationProfiles();
    json saveQuantizationProfile(const std::string& id, const json& body);
    bool deleteQuantizationProfile(const std::string& id, std::string& error);
    json benchmarks(int limit);
    json saveBenchmark(const std::string& path, const std::string& modelId, const json& metrics, const std::string& raw, const std::string& requestedQuant = "");
    json listWorkflows();
    json getWorkflow(const std::string& id);
    json saveWorkflow(const std::string& id, const json& body);
    bool deleteWorkflow(const std::string& id, std::string& error);

private:
    Database& db_;
    SettingsService& settings_;
    ConverterService& converter_;
    TaskManager& tasks_;
    std::mutex resourceMutex_;
    uint64_t previousCpuIdle_ = 0;
    uint64_t previousCpuTotal_ = 0;
    int64_t previousCpuSampleMs_ = 0;
    json modelFromRow(const json& row, bool includeFiles);
    void refreshCpu(uint64_t& idle, uint64_t& total);
};

} // namespace mf
