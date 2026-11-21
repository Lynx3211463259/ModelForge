#pragma once

#include "modelforge/util.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mf {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequestOptions {
    std::string method = "GET";
    std::vector<HttpHeader> headers;
    std::string body;
    int connectTimeoutMs = 15000;
    int receiveTimeoutMs = 60000;
    int maxRedirects = 6;
    std::atomic<bool>* abortFlag = nullptr;
};

struct HttpResponse {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    std::string finalUrl;
    std::vector<HttpHeader> headers;

    std::string header(const std::string& name) const;
    bool retryable() const;
    bool authFailure() const;
};

// Simple token-bucket style limiter shared by every worker of a download task.
// A rate of 0 means unlimited.
class SpeedLimiter {
public:
    void setRate(uint64_t bytesPerSecond);
    uint64_t rate() const;
    // Blocks until the requested amount can be transferred. Returns false when cancelled.
    bool acquire(uint64_t bytes, const std::atomic<bool>& abortFlag);
    void wakeAll();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t rate_ = 0;
    double available_ = 0.0;
    int64_t lastRefillMs_ = 0;
};

struct HttpDownloadOptions {
    std::string url;
    std::string method = "GET";
    std::vector<HttpHeader> headers;
    fs::path targetPath;
    uint64_t resumeOffset = 0;
    int connectTimeoutMs = 15000;
    int receiveTimeoutMs = 60000;
    int maxRedirects = 6;
    SpeedLimiter* limiter = nullptr;
    std::atomic<bool>* abortFlag = nullptr;
    // bytesThisSession: bytes appended during this call. contentBytes: response body length when known.
    std::function<void(uint64_t bytesThisSession, uint64_t contentBytes)> onProgress;
};

struct HttpDownloadResult {
    bool ok = false;
    bool aborted = false;
    int status = 0;
    uint64_t bytesWritten = 0;
    uint64_t contentLength = 0;
    bool rangeHonored = false;
    std::string etag;
    std::string lastModified;
    std::string error;
    std::string finalUrl;
    bool authFailure() const;
    bool retryable() const;
};

class HttpClient {
public:
    static HttpResponse request(const std::string& url, const HttpRequestOptions& options = {});
    static HttpDownloadResult download(const HttpDownloadOptions& options);
    // Removes access tokens from any text that may be logged or shown to the user.
    static std::string redact(const std::string& text);
};

} // namespace mf
