#pragma once

#include "modelforge/util.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mf {

// Pure helpers are kept public so the unit tests can verify version handling
// without any network access.
namespace updates {

// Extracts numeric components from tags such as "v1.2.0" or "1.10.0-beta".
bool parseVersion(const std::string& value, std::vector<int>& parts);
// Returns -1 / 0 / 1. Malformed versions compare equal so a bad tag can never
// trigger a false upgrade.
int compareVersions(const std::string& left, const std::string& right);
// "win64", "linux-x64", "macos-arm64", "android-arm64" or "openeuler-x64".
// MODELFORGE_UPDATE_ASSET / MODELFORGE_UPDATE_PLATFORM can override it for tests.
std::string platformAssetSuffix();
// Reads "<sha256>  <filename>" lines from a SHA256SUMS document.
std::string checksumFor(const std::string& sha256sums, const std::string& fileName);
// Asset names that may belong to a release: the exact version (including a
// pre-release suffix, "1.1.0-beta.1") first, then the numeric prefix ("1.1.0").
// Published archives use whichever form the VERSION file had at build time.
std::vector<std::string> candidateAssetNames(const std::string& version, const std::string& suffix);
// Normalizes the update channel: "stable" | "nightly" | "auto".
// Unknown values fall back to "stable".
std::string normalizeChannel(const std::string& channel);
// Picks the index of the best release from a GitHub /releases array.
// "stable" ignores prereleases, "nightly" prefers prereleases and falls back
// to stable when none exists, "auto" takes the highest version of any kind.
// Returns -1 when no usable release is present. Pure so tests can cover it.
int selectReleaseIndex(const json& releases, const std::string& channel);

} // namespace updates

struct UpdateInfo {
    std::string currentVersion;
    std::string latestVersion;
    std::string releaseName;
    std::string releaseNotes;
    std::string htmlUrl;
    std::string publishedAt;
    bool updateAvailable = false;
    bool prerelease = false;
    // Channel used for this check: stable | nightly | auto.
    std::string channel;
    std::string assetName;
    std::string assetUrl;
    uint64_t assetSize = 0;
    // Empty on success. UP_TO_DATE is a successful no-op, not an error.
    std::string code;
    std::string message;
    std::string suggestion;

    json toJson() const;
};

struct UpdateJob {
    std::string id;
    std::string status = "idle";
    std::string statusLabel = "空闲";
    int progress = 0;
    std::string message;
    std::string code;
    std::string version;
    std::string assetName;
    uint64_t downloadedBytes = 0;
    uint64_t totalBytes = 0;
    std::string scriptPath;

    json toJson() const;
};

class UpdateService {
public:
    UpdateService(const fs::path& installRoot, const fs::path& tempDir);

    // channel: stable | nightly | auto (see updates::normalizeChannel).
    UpdateInfo check(const std::string& channel);
    UpdateInfo lastCheck() const;
    std::string startDownload(std::string& code, std::string& message);
    UpdateJob job(const std::string& id) const;
    bool apply(const std::string& id, std::string& code, std::string& message, std::string& scriptPath);

private:
    fs::path installRoot_;
    fs::path tempDir_;
    mutable std::mutex mutex_;
    std::map<std::string, UpdateJob> jobs_;
    UpdateInfo lastCheck_;

    void store(const std::string& id, const UpdateJob& value);
    UpdateJob load(const std::string& id) const;
    void runDownload(const std::string& id, const UpdateInfo& info);
    void failJob(const std::string& id, const std::string& code, const std::string& message);
    std::string writeApplyScript(const UpdateJob& job, const fs::path& sourceDir) const;
};

} // namespace mf
