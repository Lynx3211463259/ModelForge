#pragma once

#include "ghc_filesystem.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mf {

struct ProcessResult {
    bool started = false;
    bool canceled = false;
    bool timedOut = false;
    int exitCode = -1;
    int64_t durationMs = 0;
    std::string error;
};

// Per-process counters sampled while the child runs. Values are cumulative for the
// process, except cpuPercent which is the utilisation during the last sample window.
struct ProcessSample {
    int64_t cpuTimeMs = 0;
    uint64_t workingSetBytes = 0;
    uint64_t peakMemoryBytes = 0;
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    double cpuPercent = 0.0;
    int64_t elapsedMs = 0;
};

struct ProcessOptions {
    std::string executable;
    std::vector<std::string> args;
    ghc::filesystem::path workingDirectory;
    std::map<std::string, std::string> environment;
    std::atomic<bool>* cancelFlag = nullptr;
    int timeoutMs = 0;
    std::function<void(const std::string& line, bool isError)> onLine;
    // Raw byte stream (used by the curl-backed HTTP transport). When set, onLine is
    // not invoked for that stream, so binary payloads are never altered by splitting.
    std::function<void(const char* data, size_t size, bool isError)> onData;
    // Called roughly every 200 ms with the counters of the child process only, so a
    // conversion task never reports host-wide numbers as its own usage.
    std::function<void(const ProcessSample& sample)> onSample;
    std::function<void()> onTick;
};

class ProcessRunner {
public:
    static ProcessResult run(const ProcessOptions& options);
};

} // namespace mf
