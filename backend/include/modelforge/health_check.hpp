#pragma once

#include "modelforge/converter.hpp"
#include "modelforge/database.hpp"
#include "modelforge/llama_runtime.hpp"
#include "modelforge/offline_library.hpp"
#include "modelforge/settings.hpp"

#include <functional>
#include <mutex>
#include <string>

namespace mf {

// Structured dependency self-check.
//
// Each item is a single actionable finding with a stable id, a severity
// (ok | warn | error), the observed value, a hint and an optional safe fix. The
// check never mutates state unless fix() is called; network probes are opt-in so
// the page stays usable fully offline.
class HealthCheckService {
public:
    // tokenProvider returns the configured Hugging Face token ("" when unset). Using a
    // callback keeps this service free of download-manager dependencies.
    HealthCheckService(Database& db, SettingsService& settings, ConverterService& converter,
                       LlamaRuntimeService& llama, OfflineLibrary& library,
                       std::function<std::string()> tokenProvider = {});

    // network: also validate the Hugging Face token reachability.
    // deep: re-detect tool versions instead of using the cached capability snapshot.
    json run(bool deep = false, bool network = false);
    // Applies a whitelisted fix and returns the refreshed item.
    json fix(const std::string& id, std::string& error);
    // Short summary for dashboards: {status, errors, warnings, checkedAt}.
    json summary();

private:
    Database& db_;
    SettingsService& settings_;
    ConverterService& converter_;
    LlamaRuntimeService& llama_;
    OfflineLibrary& library_;
    std::function<std::string()> tokenProvider_;
    std::mutex mutex_;

    json last_ = json::object();
    json checkAll(bool deep, bool network);
    static json makeItem(const std::string& id, const std::string& category, const std::string& title,
                         const std::string& status, const std::string& detail, const std::string& hint,
                         const std::string& fix = "");
    static json buildReport(const json& items);
};

} // namespace mf
