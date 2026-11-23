#include "modelforge/update_service.hpp"

#include "modelforge/crypto.hpp"
#include "modelforge/http_client.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/process_runner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef MODELEFORGE_VERSION
#define MODELEFORGE_VERSION "0.0.0"
#endif

namespace mf {

namespace updates {

bool parseVersion(const std::string& value, std::vector<int>& parts) {
    parts.clear();
    size_t i = 0;
    while (i < value.size() && !std::isdigit(static_cast<unsigned char>(value[i]))) ++i;
    if (i >= value.size()) return false;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
        int number = 0;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
            number = number * 10 + (value[i] - '0');
            ++i;
        }
        parts.push_back(number);
        if (i < value.size() && value[i] == '.') { ++i; continue; }
        break;
    }
    return !parts.empty();
}

int compareVersions(const std::string& left, const std::string& right) {
    std::vector<int> a;
    std::vector<int> b;
    if (!parseVersion(left, a) || !parseVersion(right, b)) return 0;
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

std::string platformAssetSuffix() {
    const char* forced = std::getenv("MODELFORGE_UPDATE_ASSET");
    if (forced && *forced) {
        const std::string value = trim(forced);
        if (!value.empty()) return value;
    }
    std::string arch = "x64";
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
    arch = "arm64";
#endif
#if defined(_WIN32)
    // The portable Windows package is a single x64 build named "win64".
    return "win64";
#elif defined(__APPLE__)
    return "macos-" + arch;
#elif defined(__ANDROID__)
    return "android-" + arch;
#else
    std::string platform = "linux";
    const char* overridePlatform = std::getenv("MODELFORGE_UPDATE_PLATFORM");
    if (overridePlatform && *overridePlatform) {
        const std::string value = trim(overridePlatform);
        if (!value.empty()) platform = value;
    }
    return platform + "-" + arch;
#endif
}

std::string checksumFor(const std::string& sha256sums, const std::string& fileName) {
    std::istringstream input(sha256sums);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string hash;
        std::string name;
        fields >> hash >> name;
        if (hash.empty() || name.empty()) continue;
        if (startsWith(toLower(hash), "sha256:")) hash = hash.substr(7);
        if (name.front() == '*') name.erase(name.begin());
        const size_t slash = name.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        if (base == fileName && hash.size() == 64) return toLower(hash);
    }
    return "";
}

std::string normalizeChannel(const std::string& channel) {
    const std::string value = toLower(trim(channel));
    if (value == "nightly" || value == "auto" || value == "stable") return value;
    return "stable";
}

int selectReleaseIndex(const json& releases, const std::string& channel) {
    if (!releases.is_array()) return -1;
    const std::string mode = normalizeChannel(channel);
    int bestAll = -1;
    int bestPre = -1;
    std::string allTag;
    std::string preTag;
    for (int i = 0; i < static_cast<int>(releases.size()); ++i) {
        const json& release = releases[static_cast<size_t>(i)];
        if (!release.is_object()) continue;
        if (release.value("draft", false)) continue;
        const std::string tag = release.value("tag_name", "");
        std::vector<int> parts;
        if (!parseVersion(tag, parts)) continue;
        const bool pre = release.value("prerelease", false);
        if (mode == "stable" && pre) continue;
        if (bestAll < 0 || compareVersions(tag, allTag) > 0) {
            bestAll = i;
            allTag = tag;
        }
        if (pre && (bestPre < 0 || compareVersions(tag, preTag) > 0)) {
            bestPre = i;
            preTag = tag;
        }
    }
    if (mode == "nightly") return bestPre >= 0 ? bestPre : bestAll;
    return bestAll;
}

} // namespace updates

namespace {

constexpr const char* kRepo = "Lynx3211463259/ModelForge";
constexpr const char* kApiBase = "https://api.github.com";

std::string apiBase() {
    const char* override = std::getenv("MODELFORGE_GITHUB_API");
    if (override && *override) {
        std::string value = trim(override);
        while (!value.empty() && value.back() == '/') value.pop_back();
        if (!value.empty()) return value;
    }
    return kApiBase;
}

std::string versionDigits(const std::string& tag) {
    std::vector<int> parts;
    if (!updates::parseVersion(tag, parts)) return "";
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back('.');
        out += std::to_string(parts[i]);
    }
    return out;
}

int currentProcessId() {
#ifdef _WIN32
    return static_cast<int>(_getpid());
#else
    return static_cast<int>(getpid());
#endif
}

#ifndef _WIN32
std::string singleQuoted(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}
#endif

bool extractArchive(const fs::path& archive, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        error = "无法创建解压目录：" + ec.message();
        return false;
    }
#ifdef _WIN32
    const fs::path tar = pathFromUtf8(getExecutablePath()).parent_path() / "tar.exe";
    ProcessOptions options;
    if (fs::is_regular_file(tar, ec)) {
        options.executable = pathToUtf8(tar);
    } else {
        options.executable = "tar.exe";
    }
    options.args = {"-xf", pathToUtf8(archive), "-C", pathToUtf8(destination)};
    options.timeoutMs = 15 * 60 * 1000;
    ProcessResult result = ProcessRunner::run(options);
    if (result.started && result.exitCode == 0 && !result.timedOut) return true;

    const std::string script = "Expand-Archive -LiteralPath '" + pathToUtf8(archive) +
                               "' -DestinationPath '" + pathToUtf8(destination) + "' -Force";
    ProcessOptions ps;
    ps.executable = "powershell.exe";
    ps.args = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script};
    ps.timeoutMs = 15 * 60 * 1000;
    result = ProcessRunner::run(ps);
    if (!result.started || result.exitCode != 0) {
        error = "解压失败：系统缺少 tar.exe 或 PowerShell 解压能力";
        return false;
    }
    return true;
#else
    ProcessOptions options;
    options.executable = "unzip";
    options.args = {"-q", "-o", pathToUtf8(archive), "-d", pathToUtf8(destination)};
    options.timeoutMs = 15 * 60 * 1000;
    const ProcessResult result = ProcessRunner::run(options);
    if (!result.started || result.exitCode != 0) {
        error = result.error.empty() ? "解压失败：需要 unzip 命令" : ("解压失败：" + result.error);
        return false;
    }
    return true;
#endif
}

std::string assetFolder(const std::string& assetName) {
    std::string value = assetName;
    const std::string prefix = "ModelForge-";
    if (startsWith(value, prefix)) value = value.substr(prefix.size());
    const size_t dash = value.find('-');
    if (dash != std::string::npos) value = value.substr(dash + 1);
    if (endsWith(value, ".zip")) value.resize(value.size() - 4);
    return value;
}

} // namespace

// ---------------------------------------------------------------------------
// JSON projections
// ---------------------------------------------------------------------------

json UpdateInfo::toJson() const {
    return json{{"currentVersion", currentVersion},
                {"latestVersion", latestVersion},
                {"releaseName", releaseName},
                {"releaseNotes", releaseNotes},
                {"htmlUrl", htmlUrl},
                {"publishedAt", publishedAt},
                {"updateAvailable", updateAvailable},
                {"prerelease", prerelease},
                {"channel", channel},
                {"assetName", assetName},
                {"assetSize", assetSize},
                {"assetSizeLabel", humanBytes(assetSize)},
                {"code", code},
                {"message", message},
                {"suggestion", suggestion}};
}

json UpdateJob::toJson() const {
    return json{{"id", id},
                {"status", status},
                {"statusLabel", statusLabel},
                {"progress", progress},
                {"message", message},
                {"code", code},
                {"version", version},
                {"assetName", assetName},
                {"downloadedBytes", downloadedBytes},
                {"totalBytes", totalBytes},
                {"scriptPath", scriptPath}};
}

// ---------------------------------------------------------------------------
// UpdateService
// ---------------------------------------------------------------------------

UpdateService::UpdateService(const fs::path& installRoot, const fs::path& tempDir)
    : installRoot_(installRoot), tempDir_(tempDir) {}

void UpdateService::store(const std::string& id, const UpdateJob& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_[id] = value;
}

UpdateJob UpdateService::load(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = jobs_.find(id);
    return it == jobs_.end() ? UpdateJob{} : it->second;
}

UpdateInfo UpdateService::lastCheck() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastCheck_;
}

UpdateJob UpdateService::job(const std::string& id) const {
    return load(id);
}

UpdateInfo UpdateService::check(const std::string& channel) {
    const std::string mode = updates::normalizeChannel(channel);
    UpdateInfo result;
    result.currentVersion = MODELEFORGE_VERSION;
    result.channel = mode;

    HttpRequestOptions options;
    options.connectTimeoutMs = 8000;
    options.receiveTimeoutMs = 15000;
    options.headers = {{"User-Agent", std::string("ModelForge/") + MODELEFORGE_VERSION},
                       {"Accept", "application/vnd.github+json"},
                       {"X-GitHub-Api-Version", "2022-11-28"}};
    // "stable" uses /releases/latest, which GitHub guarantees to be a
    // non-prerelease. The other channels need the full list so prereleases and
    // nightly builds can be taken into account.
    const bool stableOnly = mode == "stable";
    const std::string url = apiBase() + std::string("/repos/") + kRepo +
                            (stableOnly ? "/releases/latest" : "/releases?per_page=40");
    const HttpResponse response = HttpClient::request(url, options);
    if (!response.ok) {
        result.code = "NETWORK_ERROR";
        result.message = "无法连接 GitHub：" + response.error;
        result.suggestion = "请检查网络或代理设置后重试。";
        return result;
    }
    if (response.status != 200) {
        result.code = "NETWORK_ERROR";
        result.message = "GitHub 返回 HTTP " + std::to_string(response.status);
        if (response.status == 403) result.suggestion = "GitHub API 可能已限流，请稍后再试。";
        else if (response.status == 404) result.suggestion = "未找到公开 Release，请确认仓库地址。";
        return result;
    }

    json release;
    try {
        const json parsed = json::parse(response.body);
        if (stableOnly) {
            if (!parsed.is_object()) throw std::runtime_error("expected release object");
            release = parsed;
        } else {
            if (!parsed.is_array()) throw std::runtime_error("expected release array");
            const int index = updates::selectReleaseIndex(parsed, mode);
            if (index < 0) {
                result.code = "NO_RELEASE";
                result.message = "所选更新通道暂无可用的版本。";
                result.suggestion = "可切换到“稳定版”通道，或前往 https://github.com/" + std::string(kRepo) +
                                    "/releases 手动查看。";
                return result;
            }
            release = parsed[static_cast<size_t>(index)];
        }
    } catch (const std::exception&) {
        result.code = "VERSION_PARSE_FAILED";
        result.message = "GitHub 返回的内容不是有效的版本信息。";
        return result;
    }
    result.latestVersion = release.value("tag_name", "");
    std::vector<int> currentParts;
    std::vector<int> latestParts;
    if (!updates::parseVersion(result.latestVersion, latestParts) ||
        !updates::parseVersion(result.currentVersion, currentParts)) {
        result.code = "VERSION_PARSE_FAILED";
        result.message = "无法解析版本号：当前 " + result.currentVersion + "，最新 " + result.latestVersion;
        result.suggestion = "请确认版本号为 x.y.z 格式。";
        return result;
    }

    result.releaseName = release.value("name", "");
    result.htmlUrl = release.value("html_url", "");
    result.publishedAt = release.value("published_at", "");
    result.prerelease = release.value("prerelease", false);
    const std::string notes = release.value("body", "");
    result.releaseNotes = notes.size() > 20000 ? notes.substr(0, 20000) + "\n\n…（已截断，完整说明见发布页）" : notes;

    const std::string digits = versionDigits(result.latestVersion);
    const std::string expected = "ModelForge-" + digits + "-" + updates::platformAssetSuffix() + ".zip";
    for (const auto& asset : release.value("assets", json::array())) {
        if (asset.value("name", "") != expected) continue;
        result.assetName = expected;
        result.assetUrl = asset.value("browser_download_url", "");
        result.assetSize = asset.value("size", 0ull);
        break;
    }
    if (result.assetUrl.empty()) {
        result.code = "ASSET_NOT_FOUND";
        result.message = "当前平台没有匹配的安装包：" + expected;
        result.suggestion = "请从发布页手动下载：" + result.htmlUrl;
        return result;
    }

    result.updateAvailable = updates::compareVersions(result.latestVersion, result.currentVersion) > 0;
    result.code = result.updateAvailable ? "UPDATE_AVAILABLE" : "UP_TO_DATE";
    result.message = result.updateAvailable ? ("发现新版本 " + result.latestVersion) : "已是最新版本";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastCheck_ = result;
    }
    return result;
}

std::string UpdateService::startDownload(std::string& code, std::string& message) {
    const UpdateInfo info = lastCheck();
    if (!info.updateAvailable || info.assetUrl.empty()) {
        code = "NO_UPDATE_READY";
        message = "请先检查更新，并在发现新版本后再下载。";
        return "";
    }
    const std::string id = newId("upd");
    UpdateJob job;
    job.id = id;
    job.status = "downloading";
    job.statusLabel = "下载中";
    job.version = info.latestVersion;
    job.assetName = info.assetName;
    job.totalBytes = info.assetSize;
    job.message = "正在下载 " + info.assetName;
    store(id, job);
    std::thread([this, id, info]() { runDownload(id, info); }).detach();
    code.clear();
    message = "已开始下载";
    return id;
}

void UpdateService::failJob(const std::string& id, const std::string& code, const std::string& message) {
    UpdateJob job = load(id);
    job.status = "failed";
    job.statusLabel = "失败";
    job.code = code;
    job.message = message;
    Logger::instance().error("Update job " + id + " failed [" + code + "]: " + message);
    store(id, job);
}

void UpdateService::runDownload(const std::string& id, const UpdateInfo& info) {
    std::error_code ec;
    const fs::path base = tempDir_ / "updates" / info.latestVersion;
    fs::create_directories(base, ec);
    if (ec) {
        failJob(id, "DOWNLOAD_FAILED", "无法创建更新目录：" + ec.message());
        return;
    }

    UpdateJob job = load(id);
    job.status = "downloading";
    job.statusLabel = "下载中";
    job.message = "正在下载 " + info.assetName;
    store(id, job);

    const fs::path archive = base / info.assetName;
    HttpDownloadOptions options;
    options.url = info.assetUrl;
    options.targetPath = archive;
    options.connectTimeoutMs = 15000;
    options.receiveTimeoutMs = 180000;
    options.headers = {{"User-Agent", std::string("ModelForge/") + MODELEFORGE_VERSION}};
    options.onProgress = [this, id](uint64_t bytes, uint64_t total) {
        UpdateJob current = load(id);
        current.downloadedBytes = bytes;
        if (total > 0) current.totalBytes = total;
        current.progress = total > 0 ? static_cast<int>((bytes * 100) / total) : 0;
        store(id, current);
    };
    const HttpDownloadResult downloaded = HttpClient::download(options);
    if (!downloaded.ok) {
        failJob(id, "DOWNLOAD_FAILED", "下载失败：" + downloaded.error);
        return;
    }

    job = load(id);
    job.status = "verifying";
    job.statusLabel = "校验中";
    job.progress = 70;
    job.downloadedBytes = downloaded.bytesWritten;
    job.message = "正在校验 SHA256";
    store(id, job);

    const std::string sumsUrl = info.assetUrl.substr(0, info.assetUrl.find_last_of('/') + 1) + "SHA256SUMS.txt";
    const fs::path sumsPath = base / "SHA256SUMS.txt";
    HttpDownloadOptions sumsOptions;
    sumsOptions.url = sumsUrl;
    sumsOptions.targetPath = sumsPath;
    sumsOptions.connectTimeoutMs = 10000;
    sumsOptions.receiveTimeoutMs = 30000;
    const HttpDownloadResult sumsResult = HttpClient::download(sumsOptions);
    if (!sumsResult.ok) {
        failJob(id, "CHECKSUM_MISSING", "无法下载校验文件 SHA256SUMS.txt：" + sumsResult.error);
        return;
    }
    const std::string expected = updates::checksumFor(readTextFile(sumsPath), info.assetName);
    if (expected.empty()) {
        failJob(id, "CHECKSUM_MISSING", "SHA256SUMS.txt 中没有 " + info.assetName + " 的校验值。");
        return;
    }
    const std::string actual = sha256File(archive);
    if (actual != expected) {
        failJob(id, "CHECKSUM_MISMATCH", "SHA256 校验失败，下载文件可能已损坏或被篡改。");
        return;
    }

    job = load(id);
    job.status = "extracting";
    job.statusLabel = "解压中";
    job.progress = 85;
    job.message = "正在解压安装包";
    store(id, job);

    std::string error;
    const fs::path extracted = base / "extracted";
    if (!extractArchive(archive, extracted, error)) {
        failJob(id, "EXTRACT_FAILED", error);
        return;
    }
    const std::string folder = assetFolder(info.assetName);
    const fs::path source = extracted / folder;
    const bool hasBinary = fs::is_regular_file(source / "modelforge", ec) ||
                           fs::is_regular_file(source / "modelforge.exe", ec);
    if (!hasBinary || folder.empty()) {
        failJob(id, "EXTRACT_FAILED", "解压后未找到 " + folder + "/modelforge 可执行文件。");
        return;
    }

    job = load(id);
    job.status = "ready";
    job.statusLabel = "已就绪";
    job.progress = 100;
    job.message = "下载与校验完成，请确认安装";
    job.scriptPath = "";
    store(id, job);
}

std::string UpdateService::writeApplyScript(const UpdateJob& job, const fs::path& sourceDir) const {
    const fs::path base = tempDir_ / "updates" / job.version;
    const int pid = currentProcessId();
    const std::string source = pathToUtf8(sourceDir);
    const std::string destination = pathToUtf8(installRoot_);
#ifdef _WIN32
    const fs::path scriptPath = base / "apply-update.bat";
    const std::string script =
        "@echo off\r\n"
        "setlocal\r\n"
        "set \"PID=" + std::to_string(pid) + "\"\r\n"
        "set \"SRC=" + source + "\"\r\n"
        "set \"DST=" + destination + "\"\r\n"
        ":wait\r\n"
        "tasklist /FI \"PID eq %PID%\" 2>NUL | findstr /I /C:\"%PID%\" >NUL\r\n"
        "if not errorlevel 1 (\r\n"
        "  ping -n 2 127.0.0.1 >NUL\r\n"
        "  goto wait\r\n"
        ")\r\n"
        "if exist \"%DST%\\modelforge.exe.bak\" del /F /Q \"%DST%\\modelforge.exe.bak\"\r\n"
        "if exist \"%DST%\\modelforge.exe\" copy /Y \"%DST%\\modelforge.exe\" \"%DST%\\modelforge.exe.bak\" >NUL\r\n"
        "xcopy /E /Y /I /Q \"%SRC%\\*\" \"%DST%\\\" >NUL\r\n"
        "if errorlevel 1 goto failed\r\n"
        "start \"\" \"%DST%\\modelforge.exe\"\r\n"
        "exit /b 0\r\n"
        ":failed\r\n"
        "echo ModelForge update failed > \"%TEMP%\\modelforge-update-failed.txt\"\r\n"
        "exit /b 1\r\n";
    writeTextFile(scriptPath, script);
    return pathToUtf8(scriptPath);
#else
    const fs::path scriptPath = base / "apply-update.sh";
    const std::string script =
        "#!/bin/sh\n"
        "PID=" + std::to_string(pid) + "\n"
        "SRC=" + singleQuoted(source) + "\n"
        "DST=" + singleQuoted(destination) + "\n"
        "while kill -0 \"$PID\" 2>/dev/null; do sleep 1; done\n"
        "cp -p \"$DST/modelforge\" \"$DST/modelforge.bak\" 2>/dev/null || true\n"
        "cp -a \"$SRC/.\" \"$DST/\" || exit 1\n"
        "chmod +x \"$DST/modelforge\" 2>/dev/null || true\n"
        "nohup \"$DST/modelforge\" serve --no-browser >/dev/null 2>&1 &\n"
        "exit 0\n";
    writeTextFile(scriptPath, script);
    chmod(scriptPath.c_str(), 0755);
    return pathToUtf8(scriptPath);
#endif
}

bool UpdateService::apply(const std::string& id, std::string& code, std::string& message,
                          std::string& scriptPath) {
    const UpdateJob current = load(id);
    if (current.status != "ready") {
        code = "UPDATE_NOT_READY";
        message = "安装包尚未准备就绪，无法应用更新。";
        return false;
    }
    const std::string folder = assetFolder(current.assetName);
    const fs::path source = tempDir_ / "updates" / current.version / "extracted" / folder;
    scriptPath = writeApplyScript(current, source);
    if (scriptPath.empty()) {
        code = "APPLY_FAILED";
        message = "无法写入更新脚本。";
        return false;
    }

#ifdef _WIN32
    // The apply script waits for this process to exit. Launching it through
    // ProcessRunner would block this HTTP handler until then (a deadlock), so
    // create a genuinely detached process and return immediately.
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::string commandLine = "cmd.exe /c start \"\" /b \"" + scriptPath + "\"";
    const bool launched = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                                         DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                                         nullptr, nullptr, &startup, &process) != 0;
    if (launched) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else {
        code = "APPLY_FAILED";
        message = "无法启动更新脚本（CreateProcess 失败）。";
        return false;
    }
#else
    // The trailing ampersand detaches the script from the HTTP request.
    const int result = std::system(("nohup " + singleQuoted(scriptPath) + " >/dev/null 2>&1 &").c_str());
    if (result != 0) {
        code = "APPLY_FAILED";
        message = "无法启动更新脚本。";
        return false;
    }
#endif

    UpdateJob job = current;
    job.status = "applied";
    job.statusLabel = "已安排";
    job.message = "更新脚本已启动，请关闭 ModelForge 以完成替换。";
    job.scriptPath = scriptPath;
    store(id, job);
    code.clear();
    message = job.message;
    return true;
}

} // namespace mf
