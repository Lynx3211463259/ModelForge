#pragma once

#include "modelforge/settings.hpp"
#include "modelforge/util.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mf {

// One downloadable file of an official llama.cpp GitHub release.
struct LlamaAsset {
    std::string name;
    std::string url;
    std::string digest;    // "sha256:<hex>" as published by the GitHub API, may be empty
    uint64_t size = 0;
    std::string backend;   // cpu | cuda | vulkan | rocm | sycl | openvino | opencl | unknown
    std::string variant;   // "cpu", "cuda-12.4", "vulkan", ...
    std::string arch;      // x64 | arm64 | other
    bool dependency = false; // cudart-* runtime package shipped next to the binaries
    bool compatible = false; // matches the current OS/arch and the selected backend chain
    json toJson() const;
};

struct LlamaRelease {
    std::string tag;
    std::string name;
    std::string publishedAt;
    std::string htmlUrl;
    bool prerelease = false;
    bool draft = false;
    std::vector<LlamaAsset> assets;
    json toJson() const;
};

// Result of an update check. ok=false means the check itself could not be completed.
struct LlamaCheckResult {
    bool ok = false;
    std::string code;
    std::string error;
    std::string suggestion;
    json data = json::object();
};

struct LlamaInstallJob {
    std::string id;
    std::string status = "idle";   // idle | downloading | verifying | extracting | completed | failed | cancelled
    std::string stage = "idle";
    std::string message;
    int progress = 0;
    uint64_t downloadedBytes = 0;
    uint64_t totalBytes = 0;
    double speed = 0;
    std::string tag;
    std::string asset;
    std::string backend;
    std::string errorCode;
    std::string errorMessage;
    std::string errorSuggestion;
    int64_t startedAt = 0;
    int64_t finishedAt = 0;
    json artifacts = json::array();
    json toJson() const;
};

// Manages the llama.cpp binaries that ship with ModelForge.
//
// ModelForge does not build llama.cpp anymore: it consumes the official GitHub
// release artifacts verbatim (no trimming, no renaming, no repacking). This
// service detects which official release is installed, checks for a newer one,
// downloads the asset that matches the current platform/backend, verifies the
// sha256 digest published by GitHub and extracts the archive unchanged.
class LlamaRuntimeService {
public:
    explicit LlamaRuntimeService(SettingsService& settings);
    ~LlamaRuntimeService();

    void stop();

    fs::path root() const;
    // x64 / arm64 for the running process.
    static std::string hostArch();
    static std::string hostPlatform();

    // Local inspection only: installed tag, backend, tool paths, integrity, issues.
    json status() const;

    // Network: resolves the newest official release that actually contains a
    // downloadable asset for this platform and compares it with the local one.
    LlamaCheckResult checkUpdate(bool force);

    // Lists the assets of one release tag (used by the "download other backend" UI).
    LlamaCheckResult releaseAssets(const std::string& tag);

    // Starts a background install/update job. options: tag, asset, includeSource, backend.
    bool install(const json& options, std::string& error);
    json jobStatus() const;
    bool cancel(std::string& error);

    // Backend preference chain used for asset matching, e.g. {cuda, vulkan, cpu}.
    std::vector<std::string> backendChain() const;
    // Detected accelerator / runtime capabilities of the machine.
    json probe() const;

    // ---- Helpers shared with SettingsService path detection ----
    // Depth limited recursive search for the first matching executable name.
    static std::string findTool(const fs::path& root, const std::vector<std::string>& names, int maxDepth = 4);
    // Reads the tag from a managed "<root>/releases/<tag-asset>" directory name.
    static std::string tagFromReleaseDir(const std::string& dirName);
    static std::string backendFromDir(const fs::path& dir);
    static int compareTags(const std::string& left, const std::string& right);
    // Parses "version: 7025 (abc123)" plus "built with ..." from llama-cli --version.
    static std::string toolVersion(const std::string& executable);

private:
    SettingsService& settings_;
    mutable std::mutex jobMutex_;
    std::shared_ptr<LlamaInstallJob> job_;
    std::shared_ptr<std::atomic<bool>> cancelFlag_;
    std::shared_ptr<std::atomic<bool>> finished_;
    std::thread worker_;

    // Cached release index for the UI; guarded by cacheMutex_.
    mutable std::mutex cacheMutex_;
    json cachedCheck_ = json::object();
    int64_t cachedAt_ = 0;

    fs::path statePath() const;
    fs::path downloadsDir() const;
    fs::path releasesDir() const;
    fs::path sourceDir() const;

    LlamaCheckResult fetchReleases();
    LlamaCheckResult fetchReleaseByTag(const std::string& tag);
    LlamaCheckResult resolveCandidate();
    void runInstall(std::shared_ptr<LlamaInstallJob> job, std::shared_ptr<std::atomic<bool>> cancel,
                    std::string tag, std::string assetName, bool includeSource);
    void setJob(const std::shared_ptr<LlamaInstallJob>& job);
    void applySettings(const fs::path& releaseDir, const std::string& tag, const std::string& assetName,
                       const std::string& backend, const std::string& sourceRoot, bool& changed);
    void addLog(const std::shared_ptr<LlamaInstallJob>& job, const std::string& message);
};

} // namespace mf
