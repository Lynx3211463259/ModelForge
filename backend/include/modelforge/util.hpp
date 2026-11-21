#pragma once

#include "ghc_filesystem.hpp"
#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mf {

namespace fs = ghc::filesystem;
using json = nlohmann::json;

constexpr uint64_t GB = 1024ull * 1024ull * 1024ull;
constexpr uint64_t MB = 1024ull * 1024ull;

std::string isoNow();
std::string isoFromMs(int64_t ms);
int64_t nowMs();
std::string newId(const std::string& prefix);
std::string trim(const std::string& s);
std::string toLower(const std::string& s);
std::string toUpper(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
std::vector<std::string> split(const std::string& s, char delimiter);
std::string join(const std::vector<std::string>& values, const std::string& separator);
std::string replaceAll(std::string value, const std::string& from, const std::string& to);

std::string readTextFile(const fs::path& path, uint64_t maxBytes = 32ull * MB);
bool writeTextFile(const fs::path& path, const std::string& content);
bool writeTextFileAtomic(const fs::path& path, const std::string& content);
json readJsonFile(const fs::path& path);

std::string humanBytes(uint64_t bytes);
std::string formatDuration(int64_t ms);
std::string sanitizeFileName(const std::string& name);
std::string sanitizeSingleLine(const std::string& value, size_t maxLength = 4096);
bool isPathInside(const fs::path& base, const fs::path& candidate);
std::string pathToUtf8(const fs::path& path);
fs::path pathFromUtf8(const std::string& value);
std::string shellQuote(const std::string& value, bool windows);
std::string joinCommandForDisplay(const std::string& exe, const std::vector<std::string>& args, bool windows);

std::string getExecutablePath();
std::string getHostName();
std::string getOsName();
uint64_t totalSystemMemory();
uint64_t availableSystemMemory();
uint64_t diskFreeBytes(const fs::path& path);
uint64_t diskTotalBytes(const fs::path& path);
int defaultThreadCount();
// Probe a TCP port on the given host (IPv4 literal or "localhost"). Windows allows a
// wildcard bind even when a loopback listener exists, so the host must match the one
// that will be bound. Returns 1 = free, 0 = in use, -1 = cannot tell.
int checkTcpPort(int port, const std::string& host = "127.0.0.1");
bool directoryWritable(const fs::path& path);
bool ensureDirectory(const fs::path& path, std::string& error);
std::string mimeTypeForPath(const fs::path& path);
std::string urlDecode(const std::string& value);
std::string base64UrlNoPadding(const std::string& value);

std::string detectGpuSummary();

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { if (fn_) fn_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    void dismiss() { fn_ = nullptr; }
private:
    std::function<void()> fn_;
};

} // namespace mf
