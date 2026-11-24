#pragma once

#include "modelforge/database.hpp"
#include "modelforge/llama_runtime.hpp"
#include "modelforge/settings.hpp"

#include <mutex>
#include <string>

namespace mf {

// Offline resource library.
//
// Keeps a single registry of every artifact that can be used without network
// access: local models, downloaded Hugging Face datasets and installed llama.cpp
// backends. The registry only stores metadata plus an absolute path; deleting a
// library entry never deletes files. Bundles can be exported (manifest + optional
// file copy) and imported on another machine, which is what makes an air-gapped
// install possible.
class OfflineLibrary {
public:
    OfflineLibrary(Database& db, SettingsService& settings, LlamaRuntimeService& llama);

    json list(const std::string& type, const std::string& query, int limit);
    json stats();
    // Re-indexes models, datasets/<repo> folders and llama.cpp backends. Optional
    // pruning removes entries whose files no longer exist.
    json rescan(bool prune = true);
    // options: destination (required), ids[], types[], copy (bool), name, includeSha256
    json exportBundle(const json& options, std::string& code, std::string& error);
    // options: basePath (remap root), verifySha256 (bool)
    json importBundle(const std::string& manifestPath, const json& options, std::string& code, std::string& error);
    json bundles(int limit);
    // Removes the registry row only; files stay on disk.
    bool forget(const std::string& id, std::string& error);
    // Checks that every registered file is still present (and reports size drift).
    json verifyAll();

private:
    Database& db_;
    SettingsService& settings_;
    LlamaRuntimeService& llama_;
    mutable std::mutex mutex_;

    json resourceRow(const json& row) const;
    std::string upsertResource(const std::string& type, const std::string& name, const std::string& version,
                              const std::string& path, uint64_t size, int fileCount, const std::string& sha256,
                              const std::string& source, const std::string& repoId, const std::string& revision,
                              const json& meta);
    static std::string stableId(const std::string& type, const std::string& path);
    static void directoryUsage(const fs::path& dir, uint64_t& bytes, int& files);
};

} // namespace mf
