#include "modelforge/http_client.hpp"

#include "modelforge/process_runner.hpp"

#ifndef MODELEFORGE_VERSION
#define MODELEFORGE_VERSION "0.0.0"
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace mf {

namespace {

#ifdef _WIN32

std::wstring widen(const std::string& value) {
    if (value.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &out[0], size);
    return out;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &out[0], size, nullptr, nullptr);
    return out;
}

struct WinHandle {
    HINTERNET handle = nullptr;
    WinHandle() = default;
    explicit WinHandle(HINTERNET value) : handle(value) {}
    ~WinHandle() { if (handle) WinHttpCloseHandle(handle); }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            if (handle) WinHttpCloseHandle(handle);
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    explicit operator bool() const { return handle != nullptr; }
};

struct ParsedUrl {
    bool https = true;
    std::string host;
    std::string path;
    int port = 443;
    std::string origin;
};

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error) {
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        error = "URL 缺少协议: " + url;
        return false;
    }
    const std::string scheme = url.substr(0, schemeEnd);
    if (scheme != "https" && scheme != "http") {
        error = "仅支持 http/https 地址";
        return false;
    }
    out.https = scheme == "https";
    out.port = out.https ? 443 : 80;
    const size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) pathStart = url.size();
    std::string authority = url.substr(hostStart, pathStart - hostStart);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        try { out.port = std::stoi(authority.substr(colon + 1)); } catch (...) { out.port = out.https ? 443 : 80; }
    } else {
        out.host = authority;
    }
    out.path = pathStart < url.size() ? url.substr(pathStart) : "/";
    out.origin = scheme + "://" + out.host + (colon == std::string::npos ? "" : authority.substr(colon));
    return !out.host.empty();
}

std::string winErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text = length && buffer ? narrow(std::wstring(buffer, length)) : ("WinHTTP 错误码 " + std::to_string(code));
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

std::string resolveLocation(const ParsedUrl& current, const std::string& location) {
    if (location.find("://") != std::string::npos) return location;
    if (!location.empty() && location[0] == '/') return current.origin + location;
    const size_t slash = current.path.rfind('/');
    const std::string base = slash == std::string::npos ? "/" : current.path.substr(0, slash + 1);
    return current.origin + base + location;
}

bool hasHeader(const std::vector<HttpHeader>& headers, const std::string& name) {
    const std::string lower = toLower(name);
    for (const auto& header : headers) {
        if (toLower(header.name) == lower) return true;
    }
    return false;
}

std::wstring buildHeaderBlock(const std::vector<HttpHeader>& headers) {
    std::wstring block;
    for (const auto& header : headers) {
        if (header.name.empty() || header.value.empty()) continue;
        block += widen(header.name + ": " + header.value + "\r\n");
    }
    return block;
}

std::string queryHeader(HINTERNET request, const std::wstring& name) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name.c_str(), WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return std::string();
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name.c_str(), &value[0], &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::string();
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return narrow(value);
}

DWORD queryStatus(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return 0;
    }
    return status;
}

void collectHeaders(HINTERNET request, std::vector<HttpHeader>& out) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return;
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return;
    }
    raw.resize(size / sizeof(wchar_t));
    std::string text = narrow(raw);
    std::istringstream stream(text);
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first) { first = false; continue; }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.push_back({line.substr(0, colon), trim(line.substr(colon + 1))});
    }
}

#else

struct ParsedUrl {
    bool https = true;
    std::string host;
    std::string path;
    int port = 443;
    std::string origin;
};

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& error) {
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) { error = "URL 缺少协议"; return false; }
    out.https = url.substr(0, schemeEnd) == "https";
    const size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) pathStart = url.size();
    out.host = url.substr(hostStart, pathStart - hostStart);
    out.path = pathStart < url.size() ? url.substr(pathStart) : "/";
    out.origin = url.substr(0, schemeEnd + 3) + out.host;
    return true;
}

#endif

} // namespace

std::string HttpResponse::header(const std::string& name) const {
    const std::string lower = toLower(name);
    for (const auto& item : headers) {
        if (toLower(item.name) == lower) return item.value;
    }
    return std::string();
}

bool HttpResponse::retryable() const {
    if (status == 408 || status == 429) return true;
    return status >= 500 && status <= 599;
}

bool HttpResponse::authFailure() const {
    return status == 401 || status == 403;
}

bool HttpDownloadResult::retryable() const {
    if (aborted) return false;
    if (status == 408 || status == 429) return true;
    if (status >= 500 && status <= 599) return true;
    return status == 0 && !error.empty();
}

bool HttpDownloadResult::authFailure() const {
    return status == 401 || status == 403;
}

void SpeedLimiter::setRate(uint64_t bytesPerSecond) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = bytesPerSecond;
    available_ = 0.0;
    lastRefillMs_ = nowMs();
    cv_.notify_all();
}

uint64_t SpeedLimiter::rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rate_;
}

void SpeedLimiter::wakeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

bool SpeedLimiter::acquire(uint64_t bytes, const std::atomic<bool>& abortFlag) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (rate_ == 0) return !abortFlag.load();
    if (lastRefillMs_ == 0) lastRefillMs_ = nowMs();
    while (true) {
        if (abortFlag.load()) return false;
        const int64_t now = nowMs();
        const double elapsed = static_cast<double>(now - lastRefillMs_) / 1000.0;
        if (elapsed > 0) {
            available_ = std::min<double>(available_ + elapsed * static_cast<double>(rate_),
                                          static_cast<double>(rate_));
            lastRefillMs_ = now;
        }
        if (available_ >= static_cast<double>(bytes)) {
            available_ -= static_cast<double>(bytes);
            return true;
        }
        const double missing = static_cast<double>(bytes) - available_;
        int waitMs = static_cast<int>(missing / static_cast<double>(rate_) * 1000.0);
        waitMs = std::max(10, std::min(waitMs, 120));
        cv_.wait_for(lock, std::chrono::milliseconds(waitMs));
    }
}

std::string HttpClient::redact(const std::string& text) {
    std::string out = text;
    size_t position = 0;
    while ((position = out.find("hf_", position)) != std::string::npos) {
        size_t end = position + 3;
        while (end < out.size() && (std::isalnum(static_cast<unsigned char>(out[end])) || out[end] == '_')) ++end;
        if (end - position >= 12) {
            out.replace(position, end - position, "hf_***REDACTED***");
            position += 17;
        } else {
            position = end;
        }
    }
    const std::string bearer = "Bearer ";
    position = 0;
    while ((position = out.find(bearer, position)) != std::string::npos) {
        size_t end = position + bearer.size();
        while (end < out.size() && !std::isspace(static_cast<unsigned char>(out[end])) && out[end] != '"' && out[end] != '\'') ++end;
        if (end > position + bearer.size()) {
            out.replace(position + bearer.size(), end - (position + bearer.size()), "***REDACTED***");
            position += bearer.size() + 13;
        } else {
            position = end;
        }
    }
    return out;
}

#ifndef _WIN32

// ---------------------------------------------------------------------------
// POSIX transport: curl subprocess
//
// Rationale: a second TLS stack (OpenSSL/libcurl linked in) would add a native
// dependency that differs per distro, whereas curl is already present on macOS,
// Linux, openEuler and Android/Termux. ProcessRunner supplies process-group
// cancellation and timeouts, and the response body is streamed as raw bytes, so
// download throttling, Range resume and progress reporting behave exactly like
// the WinHTTP implementation on Windows.
// ---------------------------------------------------------------------------

namespace {

struct CurlSpec {
    std::string url;
    std::string method = "GET";
    std::vector<HttpHeader> headers;
    std::string body;
    int connectTimeoutMs = 15000;
    int receiveTimeoutMs = 60000;
    int maxRedirects = 6;
};

fs::path tempHeaderFile() {
    return fs::temp_directory_path() / (newId("mf-curl") + ".headers");
}

ProcessResult runCurl(const CurlSpec& spec, const fs::path& headerFile, std::atomic<bool>& cancel,
                      const std::function<void(const char*, size_t, bool)>& onData) {
    ProcessOptions options;
    options.executable = "curl";
    options.args = {"--silent", "--show-error", "--location",
                    "--max-redirs", std::to_string(std::max(1, spec.maxRedirects)),
                    "--connect-timeout", std::to_string(std::max(1, spec.connectTimeoutMs / 1000)),
                    "--max-time", std::to_string(std::max(1, (spec.connectTimeoutMs + spec.receiveTimeoutMs) / 1000)),
                    "--dump-header", pathToUtf8(headerFile),
                    "--output", "-",
                    "--request", spec.method};
    for (const auto& header : spec.headers) {
        options.args.insert(options.args.end(), {"-H", header.name + ": " + header.value});
    }
    if (!spec.body.empty()) {
        options.args.insert(options.args.end(), {"--data-raw", spec.body});
    }
    options.cancelFlag = &cancel;
    options.onData = onData;
    options.timeoutMs = (spec.connectTimeoutMs + spec.receiveTimeoutMs) * 2 + 60000;
    return ProcessRunner::run(options);
}

struct HeaderInfo {
    int status = 0;
    uint64_t contentLength = 0;
    std::string etag;
    std::string lastModified;
    std::vector<HttpHeader> headers;
};

HeaderInfo parseHeaders(const std::string& text) {
    HeaderInfo info;
    // curl --dump-header appends one block per redirect; only the last block matters.
    std::string block = text;
    const size_t marker = text.rfind("\r\n\r\n");
    if (marker != std::string::npos) block = text.substr(marker + 4);
    std::istringstream stream(block);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (startsWith(line, "HTTP/")) {
            const auto space = line.find(' ');
            if (space != std::string::npos) info.status = std::atoi(line.c_str() + space + 1);
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = line.substr(0, colon);
        const std::string value = trim(line.substr(colon + 1));
        info.headers.push_back(HttpHeader{name, value});
        const std::string lower = toLower(name);
        if (lower == "content-length") info.contentLength = std::strtoull(value.c_str(), nullptr, 10);
        if (lower == "etag") info.etag = value;
        if (lower == "last-modified") info.lastModified = value;
    }
    return info;
}

HeaderInfo readHeaderFile(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return {};
    return parseHeaders(readTextFile(file, 4ull * MB));
}

} // namespace

HttpResponse HttpClient::request(const std::string& url, const HttpRequestOptions& options) {
    HttpResponse response;
    std::atomic<bool> cancel{false};
    const fs::path headerFile = tempHeaderFile();
    CurlSpec spec;
    spec.url = url;
    spec.method = options.method;
    spec.headers = options.headers;
    spec.body = options.body;
    spec.connectTimeoutMs = options.connectTimeoutMs;
    spec.receiveTimeoutMs = options.receiveTimeoutMs;
    spec.maxRedirects = options.maxRedirects;
    std::string stderrTail;
    const ProcessResult result = runCurl(spec, headerFile, cancel,
        [&](const char* data, size_t size, bool isError) {
            if (options.abortFlag && options.abortFlag->load()) {
                cancel.store(true);
                return;
            }
            if (isError) {
                stderrTail.append(data, size);
                if (stderrTail.size() > 4096) stderrTail.erase(0, stderrTail.size() - 4096);
            } else {
                response.body.append(data, size);
            }
        });
    const HeaderInfo headers = readHeaderFile(headerFile);
    std::error_code ec;
    fs::remove(headerFile, ec);

    response.status = headers.status;
    response.headers = headers.headers;
    response.finalUrl = url;
    response.ok = result.started && result.exitCode == 0 && !result.canceled &&
                  headers.status >= 200 && headers.status < 300;
    if (!result.started) {
        response.error = "curl 不可用，无法发起 HTTPS 请求。请安装 curl 后重试（macOS/Linux/openEuler/Termux 均已内置或可用包管理器安装）。";
    } else if (result.canceled || (options.abortFlag && options.abortFlag->load())) {
        response.error = "请求已取消";
    } else if (result.exitCode != 0) {
        response.error = "curl 退出码 " + std::to_string(result.exitCode) +
                         (stderrTail.empty() ? "" : "：" + sanitizeSingleLine(stderrTail, 400));
    }
    return response;
}

HttpDownloadResult HttpClient::download(const HttpDownloadOptions& options) {
    HttpDownloadResult result;
    std::atomic<bool> cancel{false};
    const fs::path headerFile = tempHeaderFile();
    CurlSpec spec;
    spec.url = options.url;
    spec.method = options.method;
    spec.headers = options.headers;
    spec.connectTimeoutMs = options.connectTimeoutMs;
    spec.receiveTimeoutMs = options.receiveTimeoutMs;
    spec.maxRedirects = options.maxRedirects;
    if (options.resumeOffset > 0) {
        spec.headers.push_back(HttpHeader{"Range", "bytes=" + std::to_string(options.resumeOffset) + "-"});
    }

    std::ofstream output;
    bool opened = false;
    uint64_t sessionBytes = 0;
    HeaderInfo headers;
    std::string stderrTail;

    auto openOutput = [&]() -> bool {
        headers = readHeaderFile(headerFile);
        // A 200 answer to a Range request means the server ignored it; restart from 0
        // instead of appending a full copy after the partial data.
        const bool append = options.resumeOffset > 0 && headers.status == 206;
        result.rangeHonored = options.resumeOffset > 0 && headers.status == 206;
        output.open(options.targetPath, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        opened = output.is_open();
        return opened;
    };

    const ProcessResult process = runCurl(spec, headerFile, cancel,
        [&](const char* data, size_t size, bool isError) {
            if (isError) {
                stderrTail.append(data, size);
                if (stderrTail.size() > 4096) stderrTail.erase(0, stderrTail.size() - 4096);
                return;
            }
            if (options.abortFlag && options.abortFlag->load()) {
                cancel.store(true);
                return;
            }
            if (options.limiter && !options.limiter->acquire(size, cancel)) {
                cancel.store(true);
                return;
            }
            if (!opened && !openOutput()) {
                cancel.store(true);
                return;
            }
            if (!output.is_open()) return;
            output.write(data, static_cast<std::streamsize>(size));
            sessionBytes += size;
            if (options.onProgress) options.onProgress(sessionBytes, headers.contentLength);
        });

    if (output.is_open()) output.close();
    if (!opened) headers = readHeaderFile(headerFile);
    std::error_code ec;
    fs::remove(headerFile, ec);

    result.status = headers.status;
    result.bytesWritten = sessionBytes;
    result.contentLength = headers.contentLength;
    result.etag = headers.etag;
    result.lastModified = headers.lastModified;
    result.finalUrl = options.url;
    result.aborted = process.canceled || cancel.load() || (options.abortFlag && options.abortFlag->load());
    result.ok = process.started && process.exitCode == 0 && !result.aborted &&
                headers.status >= 200 && headers.status < 300;
    if (!process.started) {
        result.error = "curl 不可用，无法下载文件。请安装 curl 后重试。";
    } else if (!opened) {
        result.error = "无法写入下载文件: " + pathToUtf8(options.targetPath);
    } else if (result.aborted) {
        result.error = "下载已取消";
    } else if (process.exitCode != 0) {
        result.error = "curl 退出码 " + std::to_string(process.exitCode) +
                       (stderrTail.empty() ? "" : "：" + sanitizeSingleLine(stderrTail, 400));
    }
    return result;
}

#else

HttpResponse HttpClient::request(const std::string& url, const HttpRequestOptions& options) {
    HttpResponse response;
    ParsedUrl parsed;
    if (!parseUrl(url, parsed, response.error)) return response;

    WinHandle session(WinHttpOpen(widen(std::string("ModelForge/") + MODELEFORGE_VERSION).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        response.error = "无法初始化 WinHTTP: " + winErrorText(GetLastError());
        return response;
    }
    WinHttpSetTimeouts(session.handle, options.connectTimeoutMs, options.connectTimeoutMs,
                       options.receiveTimeoutMs, options.receiveTimeoutMs);

    std::string currentUrl = url;
    for (int redirect = 0; redirect <= std::max(1, options.maxRedirects); ++redirect) {
        if (options.abortFlag && options.abortFlag->load()) {
            response.error = "请求已取消";
            return response;
        }
        ParsedUrl target;
        if (!parseUrl(currentUrl, target, response.error)) return response;
        WinHandle connection(WinHttpConnect(session.handle, widen(target.host).c_str(),
                                           static_cast<INTERNET_PORT>(target.port), 0));
        if (!connection) {
            response.error = "无法连接 " + target.host + ": " + winErrorText(GetLastError());
            return response;
        }
        const std::wstring method = widen(options.method);
        WinHandle request(WinHttpOpenRequest(connection.handle, method.c_str(), widen(target.path).c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             target.https ? WINHTTP_FLAG_SECURE : 0));
        if (!request) {
            response.error = "无法创建 HTTP 请求: " + winErrorText(GetLastError());
            return response;
        }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request.handle, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

        std::vector<HttpHeader> headers = options.headers;
        if (!hasHeader(headers, "User-Agent")) headers.push_back({"User-Agent", "ModelForge/" MODELEFORGE_VERSION});
        if (!hasHeader(headers, "Accept")) headers.push_back({"Accept", "application/json"});
        const std::wstring headerBlock = buildHeaderBlock(headers);
        const void* body = options.body.empty() ? nullptr : options.body.data();
        const DWORD bodyLength = static_cast<DWORD>(options.body.size());
        if (!WinHttpSendRequest(request.handle, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                headerBlock.empty() ? 0 : static_cast<DWORD>(-1), const_cast<void*>(body), bodyLength,
                                bodyLength, 0)) {
            response.error = "发送请求失败: " + winErrorText(GetLastError());
            return response;
        }
        if (!WinHttpReceiveResponse(request.handle, nullptr)) {
            response.error = "读取响应失败: " + winErrorText(GetLastError());
            return response;
        }
        const DWORD status = queryStatus(request.handle);
        if (status >= 300 && status < 400) {
            const std::string location = queryHeader(request.handle, L"Location");
            if (!location.empty() && redirect < options.maxRedirects) {
                currentUrl = resolveLocation(target, location);
                continue;
            }
        }
        response.status = static_cast<int>(status);
        response.finalUrl = currentUrl;
        collectHeaders(request.handle, response.headers);
        std::string bodyText;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
            if (options.abortFlag && options.abortFlag->load()) {
                response.error = "请求已取消";
                return response;
            }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request.handle, &chunk[0], available, &read) || read == 0) break;
            bodyText.append(chunk.data(), read);
            if (bodyText.size() > 16ull * 1024 * 1024) {
                response.error = "响应体超过 16 MB 限制";
                return response;
            }
        }
        response.body = std::move(bodyText);
        response.ok = status >= 200 && status < 300;
        if (!response.ok && response.body.empty()) {
            response.error = "HTTP " + std::to_string(status);
        }
        return response;
    }
    response.error = "重定向次数过多";
    return response;
}

HttpDownloadResult HttpClient::download(const HttpDownloadOptions& options) {
    HttpDownloadResult result;
    ParsedUrl parsed;
    if (!parseUrl(options.url, parsed, result.error)) return result;

    WinHandle session(WinHttpOpen(widen(std::string("ModelForge/") + MODELEFORGE_VERSION).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        result.error = "无法初始化 WinHTTP: " + winErrorText(GetLastError());
        return result;
    }
    WinHttpSetTimeouts(session.handle, options.connectTimeoutMs, options.connectTimeoutMs,
                       options.receiveTimeoutMs, options.receiveTimeoutMs);

    std::string currentUrl = options.url;
    for (int redirect = 0; redirect <= std::max(1, options.maxRedirects); ++redirect) {
        if (options.abortFlag && options.abortFlag->load()) {
            result.aborted = true;
            result.error = "下载已取消";
            return result;
        }
        ParsedUrl target;
        if (!parseUrl(currentUrl, target, result.error)) return result;
        WinHandle connection(WinHttpConnect(session.handle, widen(target.host).c_str(),
                                           static_cast<INTERNET_PORT>(target.port), 0));
        if (!connection) {
            result.error = "无法连接 " + target.host + ": " + winErrorText(GetLastError());
            return result;
        }
        WinHandle request(WinHttpOpenRequest(connection.handle, widen(options.method).c_str(), widen(target.path).c_str(),
                                             nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             target.https ? WINHTTP_FLAG_SECURE : 0));
        if (!request) {
            result.error = "无法创建下载请求: " + winErrorText(GetLastError());
            return result;
        }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request.handle, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

        std::vector<HttpHeader> headers = options.headers;
        if (!hasHeader(headers, "User-Agent")) headers.push_back({"User-Agent", "ModelForge/" MODELEFORGE_VERSION});
        if (!hasHeader(headers, "Accept")) headers.push_back({"Accept", "application/octet-stream"});
        std::wstring rangeValue;
        if (options.resumeOffset > 0 && !hasHeader(headers, "Range")) {
            rangeValue = L"bytes=" + std::to_wstring(options.resumeOffset) + L"-";
            headers.push_back({"Range", narrow(rangeValue)});
        }
        const std::wstring headerBlock = buildHeaderBlock(headers);
        if (!WinHttpSendRequest(request.handle, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                headerBlock.empty() ? 0 : static_cast<DWORD>(-1), nullptr, 0, 0, 0)) {
            result.error = "发送下载请求失败: " + winErrorText(GetLastError());
            return result;
        }
        if (!WinHttpReceiveResponse(request.handle, nullptr)) {
            result.error = "读取下载响应失败: " + winErrorText(GetLastError());
            return result;
        }
        const DWORD status = queryStatus(request.handle);
        if (status >= 300 && status < 400) {
            const std::string location = queryHeader(request.handle, L"Location");
            if (!location.empty() && redirect < options.maxRedirects) {
                const bool sameHost = parseUrl(resolveLocation(target, location), parsed, result.error) &&
                                      parsed.host == target.host;
                if (!sameHost) {
                    auto& list = headers;
                    list.erase(std::remove_if(list.begin(), list.end(), [](const HttpHeader& header) {
                        return toLower(header.name) == "authorization";
                    }), list.end());
                }
                currentUrl = resolveLocation(target, location);
                continue;
            }
        }

        result.status = static_cast<int>(status);
        result.finalUrl = currentUrl;
        result.etag = queryHeader(request.handle, L"ETag");
        result.lastModified = queryHeader(request.handle, L"Last-Modified");
        const std::string contentLengthText = queryHeader(request.handle, L"Content-Length");
        if (!contentLengthText.empty()) {
            try { result.contentLength = std::stoull(contentLengthText); } catch (...) {}
        }
        if (status >= 400) {
            std::string message = "HTTP " + std::to_string(status);
            if (status == 401) message = "需要 Hugging Face 访问 Token（HTTP 401）";
            else if (status == 403) message = "仓库需要授权或接受使用协议（HTTP 403）";
            else if (status == 404) message = "文件不存在（HTTP 404）";
            else if (status == 429) message = "请求被限流（HTTP 429）";
            result.error = message;
            return result;
        }

        const bool append = options.resumeOffset > 0 && status == 206;
        result.rangeHonored = append;
        if (options.resumeOffset > 0 && status == 200) {
            result.error = "服务器未按 Range 返回，已回退为重新下载";
        }
        std::ofstream output(options.targetPath.u8string(),
                             std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        if (!output) {
            result.error = "无法写入本地文件: " + pathToUtf8(options.targetPath);
            return result;
        }
        const uint64_t baseBytes = append ? options.resumeOffset : 0;
        uint64_t written = 0;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
            if (options.abortFlag && options.abortFlag->load()) {
                output.flush();
                result.bytesWritten = written;
                result.aborted = true;
                result.error = "下载已取消";
                return result;
            }
            if (options.limiter && options.abortFlag) {
                if (!options.limiter->acquire(available, *options.abortFlag)) {
                    output.flush();
                    result.bytesWritten = written;
                    result.aborted = true;
                    result.error = "下载已取消";
                    return result;
                }
            }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request.handle, &chunk[0], available, &read) || read == 0) break;
            output.write(chunk.data(), static_cast<std::streamsize>(read));
            written += read;
            if (options.onProgress) options.onProgress(written, result.contentLength + baseBytes);
        }
        output.flush();
        if (!output.good()) {
            result.error = "写入本地文件失败: " + pathToUtf8(options.targetPath);
            result.bytesWritten = written;
            return result;
        }
        result.bytesWritten = written;
        result.ok = true;
        return result;
    }
    result.error = "重定向次数过多";
    return result;
}

#endif

} // namespace mf
