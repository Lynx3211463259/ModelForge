#pragma once

#include "modelforge/database.hpp"
#include "modelforge/types.hpp"
#include "modelforge/util.hpp"

#include <mutex>
#include <string>

namespace mf {

class SettingsService {
public:
    SettingsService(Database& db, const fs::path& root, const fs::path& dataDir = {},
                    const fs::path& runtimeRoot = {});

    void load();
    AppSettings get() const;
    json update(const json& patch, std::string& error);
    json exportSettings() const;
    bool importSettings(const json& value, std::string& error);
    fs::path root() const;
    fs::path runtimeRoot() const;
    fs::path dataDir() const;
    fs::path logsDir() const;
    fs::path tempDir() const;
    fs::path outputDir() const;
    fs::path modelsDir() const;
    void save();

private:
    Database& db_;
    fs::path root_;
    fs::path runtimeRoot_;
    fs::path dataOverride_;
    mutable std::mutex mutex_;
    AppSettings settings_;

    AppSettings makeDefaults() const;
    std::string detectQuantizePath(const fs::path& llamaBase) const;
    std::string detectCliPath(const fs::path& llamaBase) const;
    std::string detectBenchPath(const fs::path& llamaBase) const;
    std::string detectServerPath(const fs::path& llamaBase) const;
    std::string detectPplPath(const fs::path& llamaBase) const;
    std::string detectWebRoot() const;
};

} // namespace mf
