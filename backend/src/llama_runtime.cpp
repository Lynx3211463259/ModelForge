#include "modelforge/llama_runtime.hpp"

#include "modelforge/crypto.hpp"
#include "modelforge/http_client.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/process_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mf {

namespace {

constexpr const char* kRepo = "ggml-org/llama.cpp";
constexpr const char* kApiBase = "https://api.github.com";
// GitHub archives a tag as a source zip that keeps the official layout
// (convert_hf_to_gguf.py, gguf-py/, requirements/, conversion/).
constexpr const char* kArchiveBase = "https://github.com";

std::string getString(const json& value, const char* key, const std::string& fallback = "") {
    if (!value.is_object()) return fallback;
    auto it = value.find(key);
    if (it == value.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

bool getBool(const json& value, const char* key, bool fallback = false) {
    if (!value.is_object()) return fallback;
    auto it = value.find(key);
    if (it == value.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

uint64_t getUint(const json& value, const char* key, uint64_t fallback = 0) {
    if (!value.is_object()) return fallback;
    auto it = value.find(key);
    if (it == value.end()) return fallback;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        const int64_t signedValue = it->get<int64_t>();
        return signedValue < 0 ? fallback : static_cast<uint64_t>(signedValue);
    }
    return fallback;
}

struct CaptureResult {
    bool ok = false;
    std::string output;
};

CaptureResult runCapture(const std::string& executable, const std::vector<std::string>& args, int timeoutMs) {
    CaptureResult capture;
    ProcessOptions options;
    options.executable = executable;
    options.args = args;
    options.timeoutMs = timeoutMs;
    options.onLine = [&](const std::string& line, bool) {
        if (!capture.output.empty()) capture.output += "\n";
        capture.output += line;
    };
    const ProcessResult result = ProcessRunner::run(options);
    capture.ok = result.started && !result.timedOut;
    return capture;
}

#ifdef _WIN32
fs::path systemDirectory() {
    wchar_t buffer[MAX_PATH] = {0};
    UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return pathFromUtf8("C:\\Windows\\System32");
    return pathFromUtf8(std::string(buffer, buffer + length));
}
#endif

// Cached GPU/Vulkan probe. Enumerating adapters through nvidia-smi takes a few
// hundred milliseconds, which is too slow for a status endpoint hit on every page load.
struct AcceleratorProbe {
    bool nvidia = false;
    bool vulkan = false;
    bool directml = false;
    std::string driverVersion;
    std::string cudaVersion;
    std::string gpuName;
};

const AcceleratorProbe& acceleratorProbe() {
    static AcceleratorProbe probe;
    static std::once_flag once;
    std::call_once(once, []() {
#ifdef _WIN32
        std::error_code ec;
        probe.vulkan = fs::is_regular_file(systemDirectory() / "vulkan-1.dll", ec);
        probe.directml = fs::is_regular_file(systemDirectory() / "DirectML.dll", ec);
        const CaptureResult smi = runCapture("nvidia-smi", {}, 6000);
        if (smi.ok && !smi.output.empty()) {
            probe.nvidia = true;
            std::smatch match;
            const std::regex driver("Driver Version\\s*:\\s*([0-9.]+)");
            if (std::regex_search(smi.output, match, driver)) probe.driverVersion = match[1];
            const std::regex cuda("CUDA Version\\s*:\\s*([0-9.]+)");
            if (std::regex_search(smi.output, match, cuda)) probe.cudaVersion = match[1];
            std::istringstream stream(smi.output);
            std::string line;
            while (std::getline(stream, line)) {
                const size_t bar = line.find('|');
                if (bar == std::string::npos) continue;
                std::string name = line;
                if (name.find("NVIDIA") != std::string::npos && name.find("Driver") == std::string::npos &&
                    name.find("CUDA") == std::string::npos) {
                    name = trim(name.substr(0, bar == 0 ? name.size() : name.find('|')));
                    if (name.find("NVIDIA") != std::string::npos) { probe.gpuName = name; break; }
                }
            }
        }
#endif
    });
    return probe;
}

std::vector<int> parseVersionParts(const std::string& value) {
    std::vector<int> parts;
    std::string digits;
    for (char c : value) {
        if (c >= '0' && c <= '9') {
            digits.push_back(c);
        } else if (!digits.empty()) {
            parts.push_back(std::atoi(digits.c_str()));
            digits.clear();
        }
    }
    if (!digits.empty()) parts.push_back(std::atoi(digits.c_str()));
    return parts;
}

// Compares two CUDA version strings such as "12.4" and "13.5".
int compareVersions(const std::string& left, const std::string& right) {
    const std::vector<int> a = parseVersionParts(left);
    const std::vector<int> b = parseVersionParts(right);
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}

int buildNumber(const std::string& tag) {
    static const std::regex pattern("^b([0-9]+)$", std::regex::icase);
    std::smatch match;
    if (std::regex_match(tag, match, pattern)) return std::atoi(match[1].str().c_str());
    return -1;
}

std::string normalizeBackend(const std::string& flavour) {
    const std::string lower = toLower(flavour);
    if (startsWith(lower, "cuda")) return "cuda";
    if (startsWith(lower, "vulkan")) return "vulkan";
    if (startsWith(lower, "rocm") || startsWith(lower, "hip")) return "rocm";
    if (startsWith(lower, "sycl")) return "sycl";
    if (startsWith(lower, "openvino")) return "openvino";
    if (startsWith(lower, "opencl")) return "opencl";
    if (startsWith(lower, "cpu")) return "cpu";
    return "unknown";
}

std::string archFromName(const std::string& name) {
    if (name.find("-arm64") != std::string::npos) return "arm64";
    if (name.find("-x64") != std::string::npos) return "x64";
    if (name.find("-s390x") != std::string::npos) return "s390x";
    return "other";
}

// Parses "llama-b12532-bin-win-cuda-12.4-x64.zip" and the matching cudart package.
bool parseAssetName(const std::string& name, LlamaAsset& asset) {
    static const std::regex llamaPattern(
        "^llama-([A-Za-z0-9.]+)-bin-([a-z0-9.]+)-(.+)-(x64|arm64|s390x)\\.(zip|tar\\.gz)$",
        std::regex::icase);
    static const std::regex cudartPattern(
        "^cudart-llama-bin-([a-z0-9.]+)-(.+)-(x64|arm64)\\.(zip|tar\\.gz)$", std::regex::icase);
    std::smatch match;
    if (std::regex_match(name, match, llamaPattern)) {
        asset.variant = match[3].str();
        asset.arch = toLower(match[4].str());
        asset.backend = normalizeBackend(asset.variant);
        asset.dependency = false;
        return true;
    }
    if (std::regex_match(name, match, cudartPattern)) {
        asset.variant = match[2].str();
        asset.arch = toLower(match[3].str());
        asset.backend = normalizeBackend(asset.variant);
        asset.dependency = true;
        return true;
    }
    return false;
}

std::vector<LlamaAsset> parseAssets(const json& array) {
    std::vector<LlamaAsset> assets;
    if (!array.is_array()) return assets;
    for (const auto& item : array) {
        if (!item.is_object()) continue;
        LlamaAsset asset;
        asset.name = getString(item, "name");
        // Accept both the GitHub API field names and ModelForge's own serialized form.
        asset.url = getString(item, "browser_download_url");
        if (asset.url.empty()) asset.url = getString(item, "url");
        asset.digest = getString(item, "digest");
        asset.size = getUint(item, "size");
        if (asset.name.empty()) continue;
        if (asset.url.empty()) continue;
        if (!parseAssetName(asset.name, asset)) {
            asset.backend = "unknown";
            asset.arch = archFromName(asset.name);
        }
        assets.push_back(asset);
    }
    return assets;
}

LlamaRelease parseRelease(const json& value) {
    LlamaRelease release;
    release.tag = getString(value, "tag_name");
    if (release.tag.empty()) release.tag = getString(value, "tag");
    release.name = getString(value, "name");
    release.publishedAt = getString(value, "published_at");
    if (release.publishedAt.empty()) release.publishedAt = getString(value, "publishedAt");
    release.htmlUrl = getString(value, "html_url");
    if (release.htmlUrl.empty()) release.htmlUrl = getString(value, "htmlUrl");
    release.prerelease = getBool(value, "prerelease");
    release.draft = getBool(value, "draft");
    if (value.contains("assets")) release.assets = parseAssets(value["assets"]);
    return release;
}

std::string tagOfArchive(const std::string& assetName) {
    // llama-b12532-bin-win-cpu-x64.zip -> b12532
    static const std::regex pattern("^llama-([A-Za-z0-9.]+)-bin-", std::regex::icase);
    std::smatch match;
    if (std::regex_search(assetName, match, pattern)) return match[1].str();
    return "";
}

// Allow pointing the update check at GitHub Enterprise or a local mirror during
// tests: MODELFORGE_GITHUB_API=https://ghe.example.com/api/v3
std::string apiBase() {
    const char* override = std::getenv("MODELFORGE_GITHUB_API");
    if (override && *override) {
        std::string value = trim(override);
        while (!value.empty() && value.back() == '/') value.pop_back();
        if (!value.empty()) return value;
    }
    return kApiBase;
}

std::string sanitizeSingleLineSafe(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    return trim(out);
}

bool extractArchive(const fs::path& archive, const fs::path& destination, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        error = "无法创建目录 " + pathToUtf8(destination) + "：" + ec.message();
        return false;
    }
#ifdef _WIN32
    // tar.exe (bsdtar) ships with Windows 10 1803+ and extracts zip archives
    // byte for byte without touching the archive itself.
    const fs::path tar = systemDirectory() / "tar.exe";
    if (fs::is_regular_file(tar, ec)) {
        ProcessOptions options;
        options.executable = pathToUtf8(tar);
        options.args = {"-xf", pathToUtf8(archive), "-C", pathToUtf8(destination)};
        options.timeoutMs = 15 * 60 * 1000;
        const ProcessResult result = ProcessRunner::run(options);
        if (result.started && result.exitCode == 0 && !result.timedOut) {
            for (fs::directory_iterator it(destination, ec), end; it != end; it.increment(ec)) return true;
        }
    }
    // Fallback for minimal images that ship without bsdtar.
    std::string script = "Expand-Archive -LiteralPath '" + pathToUtf8(archive) + "' -DestinationPath '" +
                         pathToUtf8(destination) + "' -Force";
    ProcessOptions ps;
    ps.executable = "powershell.exe";
    ps.args = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script};
    ps.timeoutMs = 15 * 60 * 1000;
    const ProcessResult result = ProcessRunner::run(ps);
    if (!result.started || result.exitCode != 0) {
        error = "解压失败：系统缺少可用的 zip 解压工具（tar.exe / PowerShell）";
        return false;
    }
    for (fs::directory_iterator it(destination, ec), end; it != end; it.increment(ec)) return true;
    error = "解压后目录为空：" + pathToUtf8(destination);
    return false;
#else
    ProcessOptions options;
    options.executable = "unzip";
    options.args = {"-q", "-o", pathToUtf8(archive), "-d", pathToUtf8(destination)};
    options.timeoutMs = 15 * 60 * 1000;
    const ProcessResult result = ProcessRunner::run(options);
    if (!result.started || result.exitCode != 0) {
        error = "解压失败：需要 unzip 命令";
        return false;
    }
    return true;
#endif
}

std::string digestHex(const std::string& digest) {
    const std::string lower = toLower(trim(digest));
    if (startsWith(lower, "sha256:")) return lower.substr(7);
    return lower;
}

std::string backendLabel(const std::string& backend) {
    if (backend == "cpu") return "CPU";
    if (backend == "cuda") return "CUDA";
    if (backend == "vulkan") return "Vulkan";
    if (backend == "rocm") return "ROCm / HIP";
    if (backend == "sycl") return "SYCL";
    if (backend == "openvino") return "OpenVINO";
    if (backend == "opencl") return "OpenCL";
    return backend.empty() ? "未知" : backend;
}

} // namespace

// ---------------------------------------------------------------------------
// JSON projections
// ---------------------------------------------------------------------------

json LlamaAsset::toJson() const {
    return json{{"name", name},
                {"url", url},
                {"digest", digest},
                {"size", size},
                {"sizeLabel", humanBytes(size)},
                {"backend", backend},
                {"backendLabel", backendLabel(backend)},
                {"variant", variant},
                {"arch", arch},
                {"dependency", dependency},
                {"compatible", compatible}};
}

json LlamaRelease::toJson() const {
    json list = json::array();
    for (const auto& asset : assets) list.push_back(asset.toJson());
    return json{{"tag", tag},
                {"name", name},
                {"publishedAt", publishedAt},
                {"htmlUrl", htmlUrl},
                {"prerelease", prerelease},
                {"draft", draft},
                {"assets", list}};
}

json LlamaInstallJob::toJson() const {
    const char* label = "空闲";
    if (status == "downloading") label = "下载中";
    else if (status == "verifying") label = "校验中";
    else if (status == "extracting") label = "解压中";
    else if (status == "completed") label = "已完成";
    else if (status == "failed") label = "失败";
    else if (status == "cancelled") label = "已取消";
    return json{{"id", id},
                {"status", status},
                {"statusLabel", label},
                {"stage", stage},
                {"message", message},
                {"progress", progress},
                {"downloadedBytes", downloadedBytes},
                {"totalBytes", totalBytes},
                {"downloadedLabel", humanBytes(downloadedBytes)},
                {"totalLabel", humanBytes(totalBytes)},
                {"speed", speed},
                {"speedLabel", humanBytes(static_cast<uint64_t>(speed)) + "/s"},
                {"tag", tag},
                {"asset", asset},
                {"backend", backend},
                {"errorCode", errorCode},
                {"errorMessage", errorMessage},
                {"errorSuggestion", errorSuggestion},
                {"startedAt", startedAt},
                {"finishedAt", finishedAt},
                {"artifacts", artifacts}};
}

// ---------------------------------------------------------------------------
// Construction and paths
// ---------------------------------------------------------------------------

LlamaRuntimeService::LlamaRuntimeService(SettingsService& settings) : settings_(settings) {}

LlamaRuntimeService::~LlamaRuntimeService() { stop(); }

void LlamaRuntimeService::stop() {
    if (cancelFlag_) cancelFlag_->store(true);
    if (worker_.joinable()) worker_.join();
}

fs::path LlamaRuntimeService::root() const {
    const AppSettings s = settings_.get();
    if (!s.llamaRoot.empty()) return pathFromUtf8(s.llamaRoot);
    return settings_.runtimeRoot() / "tools" / "llama.cpp";
}

fs::path LlamaRuntimeService::statePath() const { return root() / "modelforge-runtime.json"; }
fs::path LlamaRuntimeService::registryPath() const { return root() / "backends.json"; }
fs::path LlamaRuntimeService::downloadsDir() const { return root() / "downloads"; }
fs::path LlamaRuntimeService::releasesDir() const { return root() / "releases"; }
fs::path LlamaRuntimeService::sourceDir() const { return root() / "source"; }

std::string LlamaRuntimeService::hostPlatform() {
#ifdef _WIN32
    return "win";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string LlamaRuntimeService::hostArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x64";
#else
    return "x64";
#endif
}

// ---------------------------------------------------------------------------
// Local inspection
// ---------------------------------------------------------------------------

std::string LlamaRuntimeService::findTool(const fs::path& base, const std::vector<std::string>& names, int maxDepth) {
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return "";
    std::vector<fs::path> level{base};
    for (int depth = 0; depth < maxDepth; ++depth) {
        std::vector<fs::path> next;
        for (const auto& dir : level) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec)) {
                    const std::string file = toLower(pathToUtf8(entry.path().filename()));
                    for (const auto& name : names) {
                        if (file == toLower(name)) return pathToUtf8(fs::absolute(entry.path()));
                    }
                } else if (entry.is_directory(ec)) {
                    const std::string child = toLower(pathToUtf8(entry.path().filename()));
                    // Cached archives never contain runnable files, and gguf-py would
                    // only waste iterations. The official source tree IS scanned because
                    // convert_hf_to_gguf.py lives in source/llama.cpp-<tag>/.
                    if (child == "downloads" || child == "artifacts" || child == "gguf-py") continue;
                    next.push_back(entry.path());
                }
            }
        }
        level.swap(next);
        if (level.empty()) break;
    }
    return "";
}

std::string LlamaRuntimeService::tagFromReleaseDir(const std::string& dirName) {
    return tagOfArchive(dirName);
}

std::string LlamaRuntimeService::backendFromDir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return "";
    std::set<std::string> dlls;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        dlls.insert(toLower(pathToUtf8(entry.path().filename())));
    }
    auto has = [&](const std::string& stem) {
        // Windows ships .dll, Linux .so and macOS .dylib; accept any of them.
        for (const char* ext : {".dll", ".so", ".dylib"}) {
            if (dlls.count(stem + ext)) return true;
            for (const auto& name : dlls) {
                if (startsWith(name, stem + ext + ".")) return true;  // libggml-cuda.so.1
            }
        }
        return false;
    };
    if (has("ggml-cuda") || has("libggml-cuda")) return "cuda";
    if (has("ggml-vulkan") || has("libggml-vulkan")) return "vulkan";
    if (has("ggml-hip") || has("ggml-rocm") || has("libggml-hip") || has("libggml-rocm")) return "rocm";
    if (has("ggml-sycl") || has("libggml-sycl")) return "sycl";
    if (has("ggml-openvino") || has("libggml-openvino")) return "openvino";
    if (has("ggml-opencl") || has("libggml-opencl")) return "opencl";
    if (has("ggml-cpu") || has("llama") || has("libggml-cpu") || has("libllama")) return "cpu";
    // Static builds and hand-copied folders carry no shared libraries: fall back to
    // the official asset name, which always embeds the backend ("-bin-win-cuda-x64").
    const std::string name = toLower(pathToUtf8(dir.filename()));
    for (const char* backend : {"cuda", "vulkan", "rocm", "hip", "sycl", "openvino", "opencl", "cpu"}) {
        const std::string token = std::string("-") + backend;
        if (name.find(token) != std::string::npos) return backend == "hip" ? "rocm" : backend;
    }
    return "";
}

int LlamaRuntimeService::compareTags(const std::string& left, const std::string& right) {
    if (left == right) return 0;
    const int la = buildNumber(left);
    const int ra = buildNumber(right);
    if (la >= 0 && ra >= 0) return la < ra ? -1 : 1;
    const std::vector<int> ls = parseVersionParts(left);
    const std::vector<int> rs = parseVersionParts(right);
    const bool leftSemver = !ls.empty() && (left[0] == 'v' || left[0] == 'V');
    const bool rightSemver = !rs.empty() && (right[0] == 'v' || right[0] == 'V');
    if (leftSemver && rightSemver) {
        const size_t count = std::max(ls.size(), rs.size());
        for (size_t i = 0; i < count; ++i) {
            const int a = i < ls.size() ? ls[i] : 0;
            const int b = i < rs.size() ? rs[i] : 0;
            if (a != b) return a < b ? -1 : 1;
        }
        return 0;
    }
    // Incomparable schemes (b-series build vs. semantic tag): caller falls back
    // to the release timestamps instead of silently claiming "up to date".
    return 0;
}

std::string LlamaRuntimeService::toolVersion(const std::string& executable) {
    if (executable.empty()) return "";
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(executable), ec)) return "";
    const CaptureResult result = runCapture(executable, {"--version"}, 8000);
    if (!result.ok || result.output.empty()) return "";
    std::smatch match;
    // Upstream prints either "version: 7025 (abc1234)" or, on the v1.0 line,
    // "version: 1.0.0-dev (build 12532, commit 08c252e22)".
    const std::regex buildPattern("\\(build\\s+([0-9]{3,})");
    if (std::regex_search(result.output, match, buildPattern)) return "b" + match[1].str();
    const std::regex pattern("version:\\s*([0-9]+)\\s*\\(([0-9a-fA-F]+)\\)");
    if (std::regex_search(result.output, match, pattern)) {
        const int build = std::atoi(match[1].str().c_str());
        // Official Windows assets report the upstream build number (b1000+). A local
        // development build reports its own tiny version and must not be mistaken for
        // an official release, otherwise update checks would compare garbage.
        if (build < 100) return "";
        return "b" + match[1].str() + " (" + match[2].str().substr(0, 7) + ")";
    }
    const std::regex loose("version:\\s*([0-9]{3,})");
    if (std::regex_search(result.output, match, loose)) return "b" + match[1].str();
    return "";
}

json LlamaRuntimeService::status() const {
    const AppSettings s = settings_.get();
    const fs::path base = root();
    std::error_code ec;

    json out = json::object();
    out["root"] = pathToUtf8(base);
    out["releasesDir"] = pathToUtf8(releasesDir());
    out["downloadsDir"] = pathToUtf8(downloadsDir());
    out["sourceDir"] = pathToUtf8(sourceDir());
    out["platform"] = hostPlatform();
    out["arch"] = hostArch();
    out["backendPreference"] = s.llamaBackend;
    out["channel"] = s.llamaChannel;
    out["repo"] = s.llamaRepo.empty() ? std::string(kRepo) : s.llamaRepo;

    json state = json::object();
    if (fs::is_regular_file(statePath(), ec)) {
        try {
            state = readJsonFile(statePath());
        } catch (...) {
            state = json::object();
        }
        if (!state.is_object()) state = json::object();
    }

    std::string tag = getString(state, "tag");
    std::string assetName = getString(state, "asset");
    std::string backend = getString(state, "backend");
    fs::path releaseDir = pathFromUtf8(getString(state, "releaseDir"));
    std::string source = "github-release";
    if (!tag.empty() && !releaseDir.empty() && !fs::is_directory(releaseDir, ec)) {
        releaseDir.clear();
        source = "state-missing-files";
    }

    // Without a state file, fall back to the managed releases/ layout so that a
    // package extracted by hand is still recognised.
    if (tag.empty() || releaseDir.empty()) {
        std::string bestTag;
        fs::path bestDir;
        if (fs::is_directory(releasesDir(), ec)) {
            for (const auto& entry : fs::directory_iterator(releasesDir(), ec)) {
                if (ec || !entry.is_directory(ec)) continue;
                const std::string candidateTag = tagFromReleaseDir(pathToUtf8(entry.path().filename()));
                if (candidateTag.empty()) continue;
                if (bestTag.empty() || compareTags(candidateTag, bestTag) > 0) {
                    bestTag = candidateTag;
                    bestDir = entry.path();
                }
            }
        }
        if (!bestDir.empty()) {
            tag = bestTag;
            releaseDir = bestDir;
            if (assetName.empty()) assetName = pathToUtf8(bestDir.filename());
            if (source == "github-release") source = "directory-scan";
        }
    }

    // Tool discovery: prefer the managed release directory, then the configured paths.
    const std::vector<std::string> quantizeNames{"llama-quantize.exe", "llama-quantize"};
    const std::vector<std::string> cliNames{"llama-cli.exe", "llama-cli", "llama.exe", "llama"};
    const std::vector<std::string> serverNames{"llama-server.exe", "llama-server"};
    const std::vector<std::string> benchNames{"llama-bench.exe", "llama-bench"};

    std::string quantize = s.quantizePath;
    std::string cli = s.llamaCliPath;
    std::string server = s.llamaServerPath;
    std::string bench = s.llamaBenchPath;
    if (!releaseDir.empty()) {
        const std::string foundQuantize = findTool(releaseDir, quantizeNames, 2);
        const std::string foundCli = findTool(releaseDir, cliNames, 2);
        if (!foundQuantize.empty()) quantize = foundQuantize;
        if (!foundCli.empty()) cli = foundCli;
        const std::string foundServer = findTool(releaseDir, serverNames, 2);
        const std::string foundBench = findTool(releaseDir, benchNames, 2);
        if (!foundServer.empty()) server = foundServer;
        if (!foundBench.empty()) bench = foundBench;
    }
    if (quantize.empty()) quantize = findTool(base, quantizeNames, 4);
    if (cli.empty()) cli = findTool(base, cliNames, 4);

    const bool quantizeOk = !quantize.empty() && fs::is_regular_file(pathFromUtf8(quantize), ec);
    const bool cliOk = !cli.empty() && fs::is_regular_file(pathFromUtf8(cli), ec);
    const bool serverOk = !server.empty() && fs::is_regular_file(pathFromUtf8(server), ec);
    const bool benchOk = !bench.empty() && fs::is_regular_file(pathFromUtf8(bench), ec);

    if (backend.empty()) {
        if (!quantize.empty()) backend = backendFromDir(pathFromUtf8(quantize).parent_path());
        if (backend.empty() && !releaseDir.empty()) backend = backendFromDir(releaseDir);
    }

    if (tag.empty() && cliOk) {
        const std::string version = toolVersion(cli);
        const std::regex buildPattern("b([0-9]+)");
        std::smatch match;
        if (std::regex_search(version, match, buildPattern)) tag = "b" + match[1].str();
        if (!tag.empty() && source == "github-release") source = "binary-version";
    }

    std::string convertScript = s.convertScript;
    std::string sourceRoot;
    if (fs::is_directory(sourceDir(), ec)) {
        const std::string found = findTool(sourceDir(), {"convert_hf_to_gguf.py"}, 3);
        if (!found.empty()) {
            convertScript = found;
            sourceRoot = pathToUtf8(pathFromUtf8(found).parent_path());
        }
    }
    const bool convertOk = !convertScript.empty() && fs::is_regular_file(pathFromUtf8(convertScript), ec);

    const bool installed = quantizeOk || cliOk;
    out["installed"] = installed;
    out["managed"] = !releaseDir.empty();
    out["tag"] = tag;
    out["versionLabel"] = tag.empty() ? std::string("未识别") : tag;
    out["asset"] = assetName;
    out["backend"] = backend;
    out["backendLabel"] = backendLabel(backend);
    out["releaseDir"] = pathToUtf8(releaseDir);
    out["detectionSource"] = installed ? source : "not-found";
    out["convertScript"] = convertScript;
    out["convertScriptAvailable"] = convertOk;
    out["sourceRoot"] = sourceRoot;
    out["tools"] = json{{"quantize", quantize},
                        {"cli", cli},
                        {"server", server},
                        {"bench", bench},
                        {"quantizeAvailable", quantizeOk},
                        {"cliAvailable", cliOk},
                        {"serverAvailable", serverOk},
                        {"benchAvailable", benchOk}};
    out["integrity"] =
        json{{"sha256", getString(state, "sha256")},
             {"size", getUint(state, "size")},
             {"verified", getBool(state, "verified")},
             {"installedAt", getString(state, "installedAt")}};

    json issues = json::array();
    if (!installed) issues.push_back("未检测到可执行的 llama.cpp（缺少 llama-quantize / llama-cli）");
    if (!convertOk) issues.push_back("未检测到 convert_hf_to_gguf.py，无法执行 Safetensors → GGUF 转换");
    if (!releaseDir.empty() && backend.empty()) issues.push_back("无法从发行目录判断加速后端（仅发现 CPU 依赖）");
    out["issues"] = issues;
    return out;
}

json LlamaRuntimeService::registry() const {
    std::error_code ec;
    json value = json::object();
    if (fs::is_regular_file(registryPath(), ec)) {
        try {
            value = readJsonFile(registryPath());
        } catch (...) {
            value = json::object();
        }
    }
    if (!value.is_object()) value = json::object();
    if (!value.contains("items") || !value["items"].is_array()) value["items"] = json::array();
    return value;
}

bool LlamaRuntimeService::writeRegistry(const json& value) const {
    return writeTextFileAtomic(registryPath(), value.dump(2));
}

void LlamaRuntimeService::upsertBackendEntry(const std::string& releaseDir, const std::string& tag,
                                             const std::string& asset, const std::string& backend,
                                             const std::string& sourceRoot, const std::string& sha256,
                                             uint64_t size) {
    const std::string id = pathFromUtf8(releaseDir).filename().u8string();
    if (id.empty()) return;
    json reg = registry();
    json items = json::array();
    std::string installedAt = isoNow();
    for (const auto& item : reg["items"]) {
        if (!item.is_object()) continue;
        if (item.value("id", "") == id) {
            const std::string previous = item.value("installedAt", "");
            if (!previous.empty()) installedAt = previous;
            continue;
        }
        json copy = item;
        copy["active"] = false;
        items.push_back(copy);
    }
    json entry{{"id", id},
               {"tag", tag},
               {"asset", asset},
               {"backend", backend},
               {"backendLabel", backendLabel(backend)},
               {"releaseDir", releaseDir},
               {"sourceRoot", sourceRoot},
               {"sha256", sha256},
               {"size", size},
               {"installedAt", installedAt},
               {"active", true}};
    items.insert(items.begin(), entry);
    reg["items"] = items;
    reg["updatedAt"] = isoNow();
    if (!writeRegistry(reg)) {
        Logger::instance().warn("[llama-runtime] 无法写入后端清单 " + pathToUtf8(registryPath()));
    }
}

json LlamaRuntimeService::backends() const {
    std::error_code ec;
    const json state = fs::is_regular_file(statePath(), ec) ? [&] {
        try { return readJsonFile(statePath()); } catch (...) { return json::object(); }
    }() : json::object();
    const std::string activeDir = pathToUtf8(fs::absolute(pathFromUtf8(getString(state, "releaseDir")), ec));
    const std::string activeTag = getString(state, "tag");

    // Registry entries first, then any release directory that predates the registry.
    json items = json::array();
    std::set<std::string> seen;
    const json reg = registry();
    for (const auto& entry : reg["items"]) {
        if (!entry.is_object()) continue;
        const std::string dir = entry.value("releaseDir", "");
        if (dir.empty()) continue;
        const fs::path releasePath = pathFromUtf8(dir);
        const std::string canonical = pathToUtf8(fs::absolute(releasePath, ec));
        if (!seen.insert(toLower(canonical)).second) continue;
        const bool present = fs::is_directory(releasePath, ec);
        const std::string cli = findTool(releasePath, {"llama-cli.exe", "llama-cli", "llama.exe", "llama"}, 2);
        const std::string quantize = findTool(releasePath, {"llama-quantize.exe", "llama-quantize"}, 2);
        json item = entry;
        item["present"] = present;
        item["usable"] = present && (!cli.empty() || !quantize.empty());
        item["cliPath"] = cli;
        item["quantizePath"] = quantize;
        item["active"] = !activeDir.empty() && toLower(canonical) == toLower(activeDir);
        item["backendLabel"] = backendLabel(entry.value("backend", ""));
        items.push_back(item);
    }
    if (fs::is_directory(releasesDir(), ec)) {
        for (const auto& dir : fs::directory_iterator(releasesDir(), ec)) {
            if (ec || !dir.is_directory(ec)) continue;
            const std::string canonical = pathToUtf8(fs::absolute(dir.path(), ec));
            if (!seen.insert(toLower(canonical)).second) continue;
            const std::string name = pathToUtf8(dir.path().filename());
            const std::string cli = findTool(dir.path(), {"llama-cli.exe", "llama-cli", "llama.exe", "llama"}, 2);
            const std::string quantize = findTool(dir.path(), {"llama-quantize.exe", "llama-quantize"}, 2);
            if (cli.empty() && quantize.empty()) continue;
            const std::string tag = tagFromReleaseDir(name);
            const std::string backend = backendFromDir(dir.path());
            items.push_back(json{{"id", name},
                                 {"tag", tag},
                                 {"asset", name},
                                 {"backend", backend},
                                 {"backendLabel", backendLabel(backend)},
                                 {"releaseDir", canonical},
                                 {"sourceRoot", ""},
                                 {"sha256", ""},
                                 {"size", 0},
                                 {"installedAt", ""},
                                 {"active", !activeDir.empty() && toLower(canonical) == toLower(activeDir)},
                                 {"present", true},
                                 {"usable", true},
                                 {"cliPath", cli},
                                 {"quantizePath", quantize},
                                 {"detected", true}});
        }
    }
    // Backends without a referenced directory are stale registry rows.
    json available = json::array();
    for (const auto& item : items) {
        if (item.value("present", false)) available.push_back(item);
    }
    json out{{"items", available}, {"count", static_cast<int>(available.size())}};
    int activeCount = 0;
    for (const auto& item : available) if (item.value("active", false)) ++activeCount;
    out["activeCount"] = activeCount;
    out["activeId"] = "";
    for (const auto& item : available) {
        if (item.value("active", false)) {
            out["activeId"] = item.value("id", "");
            break;
        }
    }
    out["activeTag"] = activeTag;
    out["registryPath"] = pathToUtf8(registryPath());
    out["releasesDir"] = pathToUtf8(releasesDir());
    return out;
}

json LlamaRuntimeService::refreshBackends() {
    std::error_code ec;
    json items = json::array();
    if (fs::is_directory(releasesDir(), ec)) {
        for (const auto& dir : fs::directory_iterator(releasesDir(), ec)) {
            if (ec || !dir.is_directory(ec)) continue;
            const std::string name = pathToUtf8(dir.path().filename());
            const std::string cli = findTool(dir.path(), {"llama-cli.exe", "llama-cli", "llama.exe", "llama"}, 2);
            const std::string quantize = findTool(dir.path(), {"llama-quantize.exe", "llama-quantize"}, 2);
            if (cli.empty() && quantize.empty()) continue;
            items.push_back(json{{"id", name},
                                 {"tag", tagFromReleaseDir(name)},
                                 {"asset", name},
                                 {"backend", backendFromDir(dir.path())},
                                 {"releaseDir", pathToUtf8(fs::absolute(dir.path(), ec))},
                                 {"sourceRoot", ""},
                                 {"sha256", ""},
                                 {"size", 0},
                                 {"installedAt", isoNow()},
                                 {"active", false}});
        }
    }
    // Keep the currently active release marked as active.
    const json current = backends();
    const std::string activeId = current.value("activeId", "");
    for (auto& item : items) {
        item["active"] = item.value("id", "") == activeId;
    }
    json reg{{"items", items}, {"updatedAt", isoNow()}, {"source", "directory-scan"}};
    writeRegistry(reg);
    return backends();
}

bool LlamaRuntimeService::activateBackend(const std::string& id, std::string& error) {
    const json list = backends();
    const json* target = nullptr;
    for (const auto& item : list["items"]) {
        if (item.value("id", "") == id) {
            target = &item;
            break;
        }
    }
    if (!target) {
        error = "未找到该后端: " + id;
        return false;
    }
    if (!target->value("present", false) || !target->value("usable", false)) {
        error = "后端文件缺失或不完整，无法启用";
        return false;
    }
    const std::string releaseDir = target->value("releaseDir", "");
    const std::string backend = target->value("backend", "");
    bool changed = false;
    applySettings(pathFromUtf8(releaseDir), target->value("tag", ""), target->value("asset", ""), backend,
                  target->value("sourceRoot", ""), changed);
    json state{{"tag", target->value("tag", "")},
               {"asset", target->value("asset", "")},
               {"backend", backend},
               {"releaseDir", releaseDir},
               {"sourceRoot", target->value("sourceRoot", "")},
               {"sha256", target->value("sha256", "")},
               {"size", target->value("size", 0ull)},
               {"verified", !target->value("sha256", "").empty()},
               {"installedAt", target->value("installedAt", "")},
               {"source", "multi-backend"},
               {"repo", settings_.get().llamaRepo.empty() ? std::string(kRepo) : settings_.get().llamaRepo},
               {"releaseUrl", ""}};
    if (!writeTextFileAtomic(statePath(), state.dump(2))) {
        error = "无法写入运行时状态文件: " + pathToUtf8(statePath());
        return false;
    }
    json reg = registry();
    for (auto& item : reg["items"]) {
        if (!item.is_object()) continue;
        item["active"] = item.value("id", "") == id;
    }
    writeRegistry(reg);
    Logger::instance().info("[llama-runtime] 已切换后端 " + id + " (" + backend + ")");
    return true;
}

bool LlamaRuntimeService::removeBackend(const std::string& id, std::string& error) {
    const json list = backends();
    json target = json::object();
    for (const auto& item : list["items"]) {
        if (item.value("id", "") == id) {
            target = item;
            break;
        }
    }
    if (target.empty()) {
        error = "未找到该后端: " + id;
        return false;
    }
    if (target.value("active", false)) {
        error = "该后端正在使用中，请先启用另一个后端再删除";
        return false;
    }
    if (list.value("count", 0) <= 1) {
        error = "至少需要保留一个可用后端";
        return false;
    }
    std::error_code ec;
    const fs::path dir = pathFromUtf8(target.value("releaseDir", ""));
    if (!dir.empty() && fs::is_directory(dir, ec)) {
        if (!isPathInside(releasesDir(), dir)) {
            error = "拒绝删除 releases 目录之外的文件: " + pathToUtf8(dir);
            return false;
        }
        fs::remove_all(dir, ec);
        if (ec) {
            error = "删除后端目录失败: " + ec.message();
            return false;
        }
    }
    json reg = registry();
    json kept = json::array();
    for (const auto& item : reg["items"]) {
        if (item.is_object() && item.value("id", "") == id) continue;
        kept.push_back(item);
    }
    reg["items"] = kept;
    writeRegistry(reg);
    Logger::instance().info("[llama-runtime] 已删除后端 " + id);
    return true;
}

json LlamaRuntimeService::probe() const {
    const AcceleratorProbe& accel = acceleratorProbe();
    json detected = json::array();
    detected.push_back("cpu");
    if (accel.nvidia) detected.push_back("cuda");
    if (accel.vulkan) detected.push_back("vulkan");
    return json{{"platform", hostPlatform()},
                {"arch", hostArch()},
                {"nvidia", accel.nvidia},
                {"nvidiaDriver", accel.driverVersion},
                {"cudaVersion", accel.cudaVersion},
                {"gpuName", accel.gpuName},
                {"vulkan", accel.vulkan},
                {"directml", accel.directml},
                {"detected", detected},
                {"chain", backendChain()},
                {"preference", settings_.get().llamaBackend}};
}

std::vector<std::string> LlamaRuntimeService::backendChain() const {
    const AcceleratorProbe& accel = acceleratorProbe();
    std::string preference = toLower(settings_.get().llamaBackend);
    if (preference.empty()) preference = "auto";
    std::vector<std::string> chain;
    auto push = [&](const std::string& value) {
        if (std::find(chain.begin(), chain.end(), value) == chain.end()) chain.push_back(value);
    };
    if (preference == "auto") {
        if (accel.nvidia) push("cuda");
        if (accel.vulkan) push("vulkan");
        push("cpu");
        return chain;
    }
    if (preference == "cuda" || preference == "rocm" || preference == "vulkan" || preference == "sycl" ||
        preference == "openvino" || preference == "opencl") {
        push(preference);
        push("cpu");
        return chain;
    }
    push("cpu");
    return chain;
}

// ---------------------------------------------------------------------------
// GitHub release queries
// ---------------------------------------------------------------------------

namespace {

HttpResponse githubGet(const std::string& url) {
    HttpRequestOptions options;
    options.receiveTimeoutMs = 30000;
    options.connectTimeoutMs = 15000;
    options.headers.push_back({"Accept", "application/vnd.github+json"});
    options.headers.push_back({"X-GitHub-Api-Version", "2022-11-28"});
    options.headers.push_back({"User-Agent", "ModelForge-LlamaRuntime"});
    return HttpClient::request(url, options);
}

LlamaCheckResult classifyHttpFailure(const HttpResponse& response) {
    LlamaCheckResult result;
    if (response.authFailure() || response.status == 404) {
        result.code = "RELEASE_NOT_FOUND";
        result.error = "找不到官方发布资源（HTTP " + std::to_string(response.status) + "）";
        result.suggestion = "确认仓库为 ggml-org/llama.cpp，或检查该版本是否已被删除。";
        return result;
    }
    if (response.status == 403 || response.status == 429) {
        result.code = "API_RATE_LIMITED";
        result.error = "GitHub API 触发限流（HTTP " + std::to_string(response.status) + "）";
        result.suggestion = "稍后再试；匿名请求每小时有配额限制。";
        return result;
    }
    if (response.status == 0) {
        result.code = "NO_NETWORK";
        result.error = "无法连接 GitHub：" + (response.error.empty() ? std::string("网络不可用") : response.error);
        result.suggestion = "检查代理、DNS 与防火墙设置后重试；离线环境可手动下载官方 zip 后放入下载目录。";
        return result;
    }
    result.code = "RELEASE_API_FAILED";
    result.error = "GitHub API 返回 HTTP " + std::to_string(response.status);
    if (!response.body.empty()) result.error += "：" + sanitizeSingleLineSafe(response.body.substr(0, 300));
    result.suggestion = "稍后重试，或改用离线安装（把官方 zip 放进下载目录）。";
    return result;
}

} // namespace

LlamaCheckResult LlamaRuntimeService::fetchReleases() {
    const AppSettings s = settings_.get();
    const std::string repo = s.llamaRepo.empty() ? std::string(kRepo) : s.llamaRepo;
    LlamaCheckResult result;
    const HttpResponse response = githubGet(apiBase() + "/repos/" + repo + "/releases?per_page=40");
    if (!response.ok || response.status != 200) return classifyHttpFailure(response);

    json parsed;
    try {
        parsed = json::parse(response.body);
    } catch (...) {
        result.code = "RELEASE_API_INVALID";
        result.error = "无法解析 GitHub 发布列表";
        result.suggestion = "稍后重试。";
        return result;
    }
    if (!parsed.is_array()) {
        result.code = "RELEASE_API_INVALID";
        result.error = "GitHub 发布列表格式不符合预期";
        return result;
    }

    json releases = json::array();
    for (const auto& item : parsed) {
        if (!item.is_object() || getBool(item, "draft")) continue;
        const LlamaRelease release = parseRelease(item);
        if (release.tag.empty()) continue;
        releases.push_back(release.toJson());
    }
    result.ok = true;
    result.data = json{{"releases", releases}};
    return result;
}

LlamaCheckResult LlamaRuntimeService::fetchReleaseByTag(const std::string& tag) {
    const AppSettings s = settings_.get();
    const std::string repo = s.llamaRepo.empty() ? std::string(kRepo) : s.llamaRepo;
    LlamaCheckResult result;
    const HttpResponse response = githubGet(apiBase() + "/repos/" + repo + "/releases/tags/" + tag);
    if (!response.ok || response.status != 200) return classifyHttpFailure(response);
    json parsed;
    try {
        parsed = json::parse(response.body);
    } catch (...) {
        result.code = "RELEASE_API_INVALID";
        result.error = "无法解析版本信息";
        return result;
    }
    if (!parsed.is_object()) {
        result.code = "RELEASE_API_INVALID";
        result.error = "版本信息格式不符合预期";
        return result;
    }
    result.ok = true;
    result.data = parseRelease(parsed).toJson();
    return result;
}

LlamaCheckResult LlamaRuntimeService::releaseAssets(const std::string& tag) {
    if (tag.empty()) {
        LlamaCheckResult result;
        result.code = "TAG_REQUIRED";
        result.error = "缺少版本号";
        result.suggestion = "先执行一次检查更新，或指定如 b12532 的官方版本号。";
        return result;
    }
    LlamaCheckResult result = fetchReleaseByTag(tag);
    if (!result.ok) return result;
    LlamaRelease release = parseRelease(result.data);
    const std::string arch = hostArch();
    const std::string platform = hostPlatform();
    const std::vector<std::string> chain = backendChain();
    for (auto& asset : release.assets) {
        const bool platformOk = asset.name.find("-" + platform + "-") != std::string::npos;
        asset.compatible = platformOk && !asset.dependency && asset.arch == arch &&
                           std::find(chain.begin(), chain.end(), asset.backend) != chain.end();
    }
    json data = release.toJson();
    data["chain"] = chain;
    data["arch"] = arch;
    data["platform"] = platform;
    result.data = data;
    return result;
}

LlamaCheckResult LlamaRuntimeService::resolveCandidate() {
    LlamaCheckResult result;
    const LlamaCheckResult releases = fetchReleases();
    if (!releases.ok) return releases;

    const std::string arch = hostArch();
    const std::string platform = hostPlatform();
    const std::vector<std::string> chain = backendChain();
    const AcceleratorProbe& accel = acceleratorProbe();

    const AppSettings appSettings = settings_.get();
    const bool stableChannel = toLower(appSettings.llamaChannel) == "stable";
    std::string stableTag;

    std::vector<LlamaRelease> parsed;
    for (const auto& item : releases.data["releases"]) {
        LlamaRelease release = parseRelease(item);
        if (release.tag.empty()) continue;
        if (stableTag.empty() && !release.prerelease) stableTag = release.tag;
        parsed.push_back(release);
    }

    // Picks the best asset of one release for the current platform/arch/backend chain.
    auto selectAsset = [&](const LlamaRelease& release, LlamaAsset& pick, std::string& backendUsed) -> bool {
        std::vector<LlamaAsset> matches;
        for (const auto& asset : release.assets) {
            if (asset.dependency) continue;
            if (asset.arch != arch) continue;
            if (asset.name.find("-" + platform + "-") == std::string::npos) continue;
            matches.push_back(asset);
        }
        for (const auto& backend : chain) {
            std::vector<LlamaAsset> candidates;
            for (const auto& asset : matches) {
                if (asset.backend == backend) candidates.push_back(asset);
            }
            if (candidates.empty()) continue;
            // Prefer the newest CUDA toolkit the installed driver can run, but never
            // fail only because the driver version is unknown.
            std::sort(candidates.begin(), candidates.end(), [](const LlamaAsset& a, const LlamaAsset& b) {
                return compareVersions(a.variant, b.variant) > 0;
            });
            pick = candidates.front();
            if (backend == "cuda" && !accel.cudaVersion.empty()) {
                bool matched = false;
                for (const auto& candidate : candidates) {
                    if (compareVersions(candidate.variant, accel.cudaVersion) <= 0) {
                        pick = candidate;
                        matched = true;
                        break;
                    }
                }
                if (!matched) pick = candidates.back();
            }
            backendUsed = backend;
            return true;
        }
        return false;
    };

    // Channel semantics: "stable" prefers releases upstream marks as stable and only
    // falls back to pre-releases (the daily b#### builds) when none can be installed.
    json chosenRelease = json::object();
    json chosenAsset = json::object();
    std::string fallbackReason;
    LlamaAsset picked;
    std::string pickedBackend;

    auto scan = [&](bool includePrerelease) -> bool {
        for (const auto& release : parsed) {
            if (!includePrerelease && release.prerelease) continue;
            LlamaAsset candidate;
            std::string backendUsed;
            if (!selectAsset(release, candidate, backendUsed)) continue;
            picked = candidate;
            pickedBackend = backendUsed;
            LlamaAsset jsonAsset = candidate;
            jsonAsset.compatible = true;
            chosenAsset = jsonAsset.toJson();
            chosenRelease = release.toJson();
            return true;
        }
        return false;
    };

    bool found = scan(true);
    if (stableChannel) {
        found = scan(false);
        if (!found) {
            found = scan(true);
            if (found) fallbackReason = "官方稳定版暂无可用资产，已回退到预发布构建 " +
                                        getString(chosenRelease, "tag") + "。";
        }
    }

    json available = json::array();
    if (found) {
        for (const auto& asset : parseRelease(chosenRelease).assets) {
            if (asset.dependency) continue;
            if (asset.arch != arch) continue;
            if (asset.name.find("-" + platform + "-") == std::string::npos) continue;
            json entry = asset.toJson();
            entry["tag"] = getString(chosenRelease, "tag");
            entry["selected"] = asset.name == picked.name;
            available.push_back(entry);
        }
        if (pickedBackend != chain.front()) {
            const std::string backendNote = "该版本未提供 " + backendLabel(chain.front()) + " 资源，已回退到 " +
                                            backendLabel(pickedBackend) + "。";
            fallbackReason = fallbackReason.empty() ? backendNote : fallbackReason + " " + backendNote;
        }
    }

    if (chosenAsset.is_null() || chosenAsset.empty()) {
        result.code = "ASSET_MISMATCH";
        result.error = "最新发布中没有适用于 " + platform + "-" + arch + " 的资源";
        result.suggestion = "可在“下载管理”中手动获取官方 zip，或切换 llamaBackend 设置。";
        result.data = json{{"status", "asset_mismatch"},
                           {"available", available},
                           {"arch", arch},
                           {"platform", platform},
                           {"chain", chain},
                           {"stableTag", stableTag}};
        return result;
    }

    const json local = status();
    const std::string remoteTag = getString(chosenRelease, "tag");
    const std::string localTag = getString(local, "tag");

    std::string status = "up_to_date";
    std::string message;
    std::string suggestion;
    if (!getBool(local, "installed")) {
        status = "not_installed";
        message = "尚未安装可用的 llama.cpp 运行时，可下载官方 " + remoteTag + " 资源。";
        suggestion = "点击“下载并安装”获取官方发行包。";
    } else if (localTag.empty()) {
        status = "local_version_unknown";
        message = "本地存在 llama.cpp 可执行文件，但无法识别官方版本号。";
        suggestion = "建议重新安装官方发行包，以获得可校验、可更新的运行时。";
    } else if (localTag == remoteTag) {
        status = "up_to_date";
        message = "已是最新官方版本 " + localTag + "。";
    } else {
        const int order = compareTags(remoteTag, localTag);
        if (order > 0) {
            status = "update_available";
            message = "发现新版本 " + remoteTag + "（本地 " + localTag + "）。";
        } else if (order < 0) {
            status = "local_newer";
            message = "本地版本 " + localTag + " 高于公开最新版 " + remoteTag + "，无需更新。";
        } else {
            const std::string remotePublished = getString(chosenRelease, "publishedAt");
            const std::string installedAt = getString(local["integrity"], "installedAt");
            if (!remotePublished.empty() && !installedAt.empty() && remotePublished > installedAt) {
                status = "update_available";
                message = "官方发布了新构建 " + remoteTag + "（版本号规则不同，按发布时间判断）。";
            } else {
                status = "up_to_date";
                message = "本地版本与官方最新构建一致。";
            }
        }
    }

    const std::string localBackend = getString(local, "backend");
    const std::string assetBackend = getString(chosenAsset, "backend");
    bool backendUpgrade = false;
    json backendHint = nullptr;
    if (!localBackend.empty() && localBackend != assetBackend) {
        backendUpgrade = localBackend == "cpu" && assetBackend != "cpu";
        backendHint = json{{"local", localBackend},
                           {"recommended", assetBackend},
                           {"label", "当前为 " + backendLabel(localBackend) + " 版本，机器更适合 " +
                                         backendLabel(assetBackend)}};
    }

    json data{{"status", status},
              {"message", message},
              {"suggestion", suggestion},
              {"fallbackReason", fallbackReason},
              {"checkedAt", isoNow()},
              {"local", local},
              {"release", chosenRelease},
              {"asset", chosenAsset},
              {"stableTag", stableTag},
              {"remoteTag", remoteTag},
              {"backendUpgrade", backendUpgrade},
              {"backendHint", backendHint},
              {"chain", chain},
              {"platform", platform},
              {"arch", arch},
              {"available", available},
              {"probe", probe()}};
    result.ok = true;
    result.data = data;
    return result;
}

LlamaCheckResult LlamaRuntimeService::checkUpdate(bool force) {
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (!force && !cachedCheck_.empty() && nowMs() - cachedAt_ < 10 * 60 * 1000) {
            LlamaCheckResult cached;
            cached.ok = true;
            cached.data = cachedCheck_;
            cached.data["cached"] = true;
            return cached;
        }
    }
    LlamaCheckResult result = resolveCandidate();
    if (result.ok) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cachedCheck_ = result.data;
        cachedAt_ = nowMs();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Install job
// ---------------------------------------------------------------------------

void LlamaRuntimeService::setJob(const std::shared_ptr<LlamaInstallJob>& job) {
    std::lock_guard<std::mutex> lock(jobMutex_);
    job_ = job;
}

json LlamaRuntimeService::jobStatus() const {
    std::lock_guard<std::mutex> lock(jobMutex_);
    if (!job_) return LlamaInstallJob().toJson();
    return job_->toJson();
}

bool LlamaRuntimeService::cancel(std::string& error) {
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        if (!job_ || job_->status == "completed" || job_->status == "failed" || job_->status == "cancelled") {
            error = "当前没有正在进行的安装任务";
            return false;
        }
    }
    if (cancelFlag_) cancelFlag_->store(true);
    return true;
}

void LlamaRuntimeService::addLog(const std::shared_ptr<LlamaInstallJob>& job, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        job->message = message;
    }
    Logger::instance().info("[llama-runtime] " + message);
}

bool LlamaRuntimeService::install(const json& options, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        if (job_ && finished_ && !finished_->load()) {
            error = "已有 llama.cpp 安装任务正在进行";
            return false;
        }
    }
    if (worker_.joinable()) worker_.join();

    std::string tag = getString(options, "tag");
    std::string assetName = getString(options, "asset");
    // The official Windows assets do not ship convert_hf_to_gguf.py, so the source
    // archive is only fetched when the local Python converter is missing.
    bool includeSource = !getBool(status(), "convertScriptAvailable");
    if (options.is_object() && options.contains("includeSource") && options["includeSource"].is_boolean()) {
        includeSource = options["includeSource"].get<bool>();
    }

    if (tag.empty() || assetName.empty()) {
        LlamaCheckResult candidate = resolveCandidate();
        if (!candidate.ok) {
            error = candidate.error;
            return false;
        }
        if (tag.empty()) tag = getString(candidate.data, "remoteTag");
        if (assetName.empty()) assetName = getString(candidate.data["asset"], "name");
        if (!getBool(candidate.data["local"], "convertScriptAvailable")) includeSource = true;
    }
    if (tag.empty() || assetName.empty()) {
        error = "缺少可下载的官方版本或资源名称";
        return false;
    }

    auto job = std::make_shared<LlamaInstallJob>();
    job->id = newId("llama");
    job->tag = tag;
    job->asset = assetName;
    job->status = "downloading";
    job->stage = "prepare";
    job->message = "准备下载 " + assetName;
    job->startedAt = nowMs();
    job->artifacts = json::array();
    setJob(job);

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto finished = std::make_shared<std::atomic<bool>>(false);
    cancelFlag_ = cancelFlag;
    finished_ = finished;

    worker_ = std::thread([this, job, cancelFlag, finished, tag, assetName, includeSource] {
        runInstall(job, cancelFlag, tag, assetName, includeSource);
        finished->store(true);
    });
    return true;
}

void LlamaRuntimeService::runInstall(std::shared_ptr<LlamaInstallJob> job, std::shared_ptr<std::atomic<bool>> cancel,
                                     std::string tag, std::string assetName, bool includeSource) {
    auto fail = [&](const std::string& code, const std::string& message, const std::string& suggestion) {
        std::lock_guard<std::mutex> lock(jobMutex_);
        job->status = "failed";
        job->stage = "failed";
        job->errorCode = code;
        job->errorMessage = message;
        job->errorSuggestion = suggestion;
        job->message = message;
        job->finishedAt = nowMs();
        Logger::instance().error("[llama-runtime] " + code + ": " + message);
    };
    auto cancelJob = [&]() {
        std::lock_guard<std::mutex> lock(jobMutex_);
        job->status = "cancelled";
        job->stage = "cancelled";
        job->message = "安装已取消";
        job->finishedAt = nowMs();
    };

    std::error_code ec;
    fs::create_directories(downloadsDir(), ec);
    fs::create_directories(releasesDir(), ec);
    if (ec) {
        fail("DIRECTORY_NOT_WRITABLE", "无法创建 llama.cpp 目录：" + ec.message(),
             "在设置中把 llamaRoot 指向可写目录。");
        return;
    }
    if (!directoryWritable(releasesDir())) {
        fail("DIRECTORY_NOT_WRITABLE", "目录不可写：" + pathToUtf8(releasesDir()),
             "更换 llamaRoot 或调整目录权限。");
        return;
    }

    // Resolve the exact asset metadata from the official release.
    addLog(job, "读取官方发布 " + tag + " 的资源列表");
    LlamaCheckResult releaseResult = fetchReleaseByTag(tag);
    if (!releaseResult.ok) {
        fail(releaseResult.code, releaseResult.error, releaseResult.suggestion);
        return;
    }
    LlamaRelease release = parseRelease(releaseResult.data);
    const LlamaAsset* target = nullptr;
    for (const auto& asset : release.assets) {
        if (asset.name == assetName) {
            target = &asset;
            break;
        }
    }
    if (!target) {
        fail("ASSET_NOT_FOUND", "该版本中没有名为 " + assetName + " 的资源",
             "刷新资源列表后重新选择；官方资源名会随版本变化。");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        job->backend = target->backend;
        job->totalBytes = target->size;
        job->progress = 1;
    }

    struct DownloadPlan {
        LlamaAsset asset;
        fs::path archive;
        bool dependency = false;
    };
    std::vector<DownloadPlan> plans;
    plans.push_back({*target, downloadsDir() / target->name, false});

    // CUDA builds need the matching cudart package; it is published as a separate
    // official asset and is extracted next to the binaries.
    if (target->backend == "cuda") {
        for (const auto& asset : release.assets) {
            if (!asset.dependency) continue;
            if (asset.backend != "cuda" || asset.arch != target->arch) continue;
            if (asset.variant != target->variant) continue;
            plans.push_back({asset, downloadsDir() / asset.name, true});
            break;
        }
    }

    uint64_t grandTotal = 0;
    for (const auto& plan : plans) grandTotal += plan.asset.size;
    if (grandTotal == 0) grandTotal = target->size;
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        job->totalBytes = grandTotal;
    }

    uint64_t completedBytes = 0;
    for (const auto& plan : plans) {
        if (cancel->load()) { cancelJob(); return; }
        // Offline friendly: a complete archive that matches the published digest is
        // reused instead of being downloaded again (the package ships one).
        const std::string expectedDigest = digestHex(plan.asset.digest);
        bool reused = false;
        if (fs::is_regular_file(plan.archive, ec)) {
            const uint64_t existingSize = fs::file_size(plan.archive, ec);
            if ((plan.asset.size == 0 || existingSize == plan.asset.size) &&
                (expectedDigest.empty() || toLower(sha256File(plan.archive)) == expectedDigest)) {
                reused = true;
            }
        }
        if (reused) {
            completedBytes += plan.asset.size;
            {
                std::lock_guard<std::mutex> lock(jobMutex_);
                job->downloadedBytes = completedBytes;
                if (grandTotal > 0) {
                    job->progress = static_cast<int>(
                        std::min<uint64_t>(99, completedBytes * 100 / grandTotal));
                }
            }
            addLog(job, "复用已校验的官方安装包 " + plan.asset.name);
        } else {
        const fs::path part = plan.archive.string() + ".part";
        uint64_t offset = 0;
        if (fs::is_regular_file(part, ec)) offset = fs::file_size(part, ec);
        if (ec) offset = 0;
        if (plan.asset.size > 0 && offset > plan.asset.size) {
            fs::remove(part, ec);
            offset = 0;
        }
        if (offset > 0 && plan.asset.size > 0 && offset == plan.asset.size) {
            // Complete partial file from a previous session.
            fs::remove(plan.archive, ec);
            fs::rename(part, plan.archive, ec);
        } else {
            addLog(job, (offset > 0 ? "继续下载 " : "开始下载 ") + plan.asset.name + "（" +
                            humanBytes(plan.asset.size) + "）");
            int attempt = 0;
            bool done = false;
            std::string lastError;
            std::string lastCode = "DOWNLOAD_FAILED";
            while (attempt < 3 && !done) {
                if (cancel->load()) { cancelJob(); return; }
                ++attempt;
                int64_t lastSample = nowMs();
                uint64_t lastBytes = offset;
                HttpDownloadOptions options;
                options.url = plan.asset.url;
                options.targetPath = part;
                options.resumeOffset = offset;
                options.abortFlag = cancel.get();
                options.receiveTimeoutMs = 60000;
                options.headers.push_back({"User-Agent", "ModelForge-LlamaRuntime"});
                options.onProgress = [&](uint64_t bytesThisSession, uint64_t contentBytes) {
                    const int64_t now = nowMs();
                    std::lock_guard<std::mutex> lock(jobMutex_);
                    const uint64_t current = offset + bytesThisSession;
                    job->downloadedBytes = completedBytes + current;
                    if (plan.asset.size > 0) {
                        job->progress = static_cast<int>(std::min<uint64_t>(
                            99, job->downloadedBytes * 100 / std::max<uint64_t>(1, grandTotal)));
                    }
                    if (now - lastSample > 500) {
                        const double seconds = (now - lastSample) / 1000.0;
                        if (seconds > 0) job->speed = (current - lastBytes) / seconds;
                        lastSample = now;
                        lastBytes = current;
                    }
                };
                const HttpDownloadResult result = HttpClient::download(options);
                if (result.ok) {
                    offset += result.bytesWritten;
                    done = true;
                    break;
                }
                if (cancel->load() || result.aborted) { cancelJob(); return; }
                lastError = result.error;
                if (result.status == 404) {
                    lastCode = "ASSET_NOT_FOUND";
                    break;
                }
                if (result.status == 403 || result.status == 429) {
                    lastCode = "API_RATE_LIMITED";
                    break;
                }
                if (result.status == 0) lastCode = "NO_NETWORK";
                else if (!result.retryable()) lastCode = "DOWNLOAD_FAILED";
                if (fs::is_regular_file(part, ec)) offset = fs::file_size(part, ec);
                std::this_thread::sleep_for(std::chrono::milliseconds(600 * attempt));
            }
            if (!done) {
                std::string message = lastError.empty() ? "下载失败" : lastError;
                std::string suggestion = "检查网络后重试；已下载的分片会保留以便续传。";
                if (lastCode == "NO_NETWORK") {
                    message = "无法连接下载服务器：" + message;
                    suggestion = "确认网络与代理可用；也可手动下载官方 zip 后放入 " + pathToUtf8(downloadsDir()) + "。";
                } else if (lastCode == "ASSET_NOT_FOUND") {
                    message = "官方资源已不可用：" + plan.asset.name;
                    suggestion = "重新检查更新以获取当前可用的资源名。";
                } else if (lastCode == "API_RATE_LIMITED") {
                    suggestion = "GitHub 限流，请稍后再试。";
                }
                fail(lastCode, message, suggestion);
                return;
            }
        }

        // Integrity: size and the sha256 digest published by GitHub.
        if (plan.asset.size > 0) {
            const uint64_t actual = fs::is_regular_file(part, ec) ? fs::file_size(part, ec) : 0;
            if (actual != plan.asset.size) {
                fs::remove(part, ec);
                fail("SIZE_MISMATCH",
                     "文件大小不符（期望 " + std::to_string(plan.asset.size) + " 字节，实际 " +
                         std::to_string(actual) + " 字节）",
                     "已删除损坏的分片，请重试下载。");
                return;
            }
        }
        const std::string& expected = expectedDigest;
        if (!expected.empty()) {
            std::lock_guard<std::mutex> lock(jobMutex_);
            job->status = "verifying";
            job->stage = "checksum";
            job->message = "校验 " + plan.asset.name + " 的 sha256";
        }
        if (!expected.empty()) {
            const std::string actual = sha256File(part);
            if (toLower(actual) != expected) {
                fs::remove(part, ec);
                fail("CHECKSUM_MISMATCH",
                     "sha256 校验失败（期望 " + expected.substr(0, 16) + "…，实际 " + actual.substr(0, 16) + "…）",
                     "安装包已损坏或被中间代理改写，已删除文件，请重试。");
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            job->status = "extracting";
            job->stage = "extract";
            job->message = "解压 " + plan.asset.name;
        }
        // Keep the untouched official archive and extract into a private folder.
        fs::remove(plan.archive, ec);
        fs::rename(part, plan.archive, ec);
        if (ec) {
            fail("EXTRACT_FAILED", "无法保存官方安装包：" + ec.message(), "检查磁盘空间与权限。");
            return;
        }
        }
        std::string stem = plan.asset.name;
        for (const char* suffix : {".tar.gz", ".zip"}) {
            if (endsWith(toLower(stem), suffix)) {
                stem = stem.substr(0, stem.size() - std::strlen(suffix));
                break;
            }
        }
        const fs::path extractDir = releasesDir() / stem;
        std::string extractError;
        if (!extractArchive(plan.archive, extractDir, extractError)) {
            fs::remove_all(extractDir, ec);
            fail("EXTRACT_FAILED", extractError, "确认系统自带 tar.exe 或 PowerShell 可用。");
            return;
        }
        completedBytes += plan.asset.size;
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            job->downloadedBytes = completedBytes;
            job->artifacts.push_back(json{{"name", plan.asset.name},
                                          {"size", plan.asset.size},
                                          {"sha256", expectedDigest},
                                          {"archive", pathToUtf8(plan.archive)},
                                          {"directory", pathToUtf8(extractDir)},
                                          {"dependency", plan.dependency}});
        }
        addLog(job, "已解压官方资源到 " + pathToUtf8(extractDir));
    }

    if (cancel->load()) { cancelJob(); return; }

    // Optional: official tag source archive so the Python converter matches the
    // exact same release (the Windows binary assets do not ship the scripts).
    std::string sourceRoot;
    if (includeSource) {
        const std::string sourceName = "llama.cpp-" + tag;
        const fs::path expectedRoot = sourceDir() / sourceName;
        std::string foundConvert = findTool(expectedRoot, {"convert_hf_to_gguf.py"}, 2);
        if (foundConvert.empty()) {
            const std::string archiveName = "llama.cpp-" + tag + ".zip";
            const std::string url = std::string(kArchiveBase) + "/ggml-org/llama.cpp/archive/refs/tags/" + tag + ".zip";
            const fs::path archive = downloadsDir() / archiveName;
            const fs::path part = archive.string() + ".part";
            uint64_t offset = fs::is_regular_file(part, ec) ? fs::file_size(part, ec) : 0;
            if (fs::is_regular_file(archive, ec)) {
                addLog(job, "复用已下载的官方源码包 " + archiveName);
            } else {
                {
                    std::lock_guard<std::mutex> lock(jobMutex_);
                    job->status = "downloading";
                    job->stage = "source";
                    job->message = "下载官方源码包 " + archiveName + "（转换脚本）";
                }
                HttpDownloadOptions options;
                options.url = url;
                options.targetPath = part;
                options.resumeOffset = offset;
                options.abortFlag = cancel.get();
                options.receiveTimeoutMs = 60000;
                options.headers.push_back({"User-Agent", "ModelForge-LlamaRuntime"});
                options.onProgress = [&](uint64_t bytesThisSession, uint64_t) {
                    std::lock_guard<std::mutex> lock(jobMutex_);
                    job->downloadedBytes = completedBytes + offset + bytesThisSession;
                };
                const HttpDownloadResult result = HttpClient::download(options);
                if (cancel->load() || result.aborted) { cancelJob(); return; }
                if (!result.ok) {
                    // The binaries are usable; only conversion is unavailable.
                    std::lock_guard<std::mutex> lock(jobMutex_);
                    job->artifacts.push_back(json{{"name", archiveName},
                                                  {"error", result.error.empty() ? "下载失败" : result.error},
                                                  {"optional", true}});
                    addLog(job, "官方源码包下载失败，转换脚本保持原状：" + result.error);
                } else {
                    fs::remove(archive, ec);
                    fs::rename(part, archive, ec);
                }
            }
            if (fs::is_regular_file(archive, ec)) {
                std::string extractError;
                if (!extractArchive(archive, sourceDir(), extractError)) {
                    addLog(job, "源码包解压失败：" + extractError);
                }
            }
            foundConvert = findTool(expectedRoot, {"convert_hf_to_gguf.py"}, 2);
        }
        if (!foundConvert.empty()) sourceRoot = pathToUtf8(pathFromUtf8(foundConvert).parent_path());
    }

    // Point the existing conversion pipeline at the freshly installed binaries.
    bool changed = false;
    const fs::path releaseDir = releasesDir() / [&] {
        std::string stem = assetName;
        for (const char* suffix : {".tar.gz", ".zip"}) {
            if (endsWith(toLower(stem), suffix)) {
                stem = stem.substr(0, stem.size() - std::strlen(suffix));
                break;
            }
        }
        return stem;
    }();
    applySettings(releaseDir, tag, assetName, target->backend, sourceRoot, changed);
    if (changed) {
        addLog(job, "已更新工具路径设置");
    }

    json state{{"tag", tag},
               {"asset", assetName},
               {"backend", target->backend},
               {"releaseDir", pathToUtf8(releaseDir)},
               {"sourceRoot", sourceRoot},
               {"sha256", digestHex(target->digest)},
               {"size", target->size},
               {"verified", !digestHex(target->digest).empty()},
               {"installedAt", isoNow()},
               {"source", "github-release"},
               {"repo", settings_.get().llamaRepo.empty() ? std::string(kRepo) : settings_.get().llamaRepo},
               {"releaseUrl", release.htmlUrl}};
    if (!writeTextFileAtomic(statePath(), state.dump(2))) {
        addLog(job, "警告：无法写入 " + pathToUtf8(statePath()));
    }
    // Keep the multi-backend registry in sync so a freshly installed backend can be
    // switched back to later without re-downloading it.
    upsertBackendEntry(pathToUtf8(releaseDir), tag, assetName, target->backend, sourceRoot,
                       digestHex(target->digest), target->size);

    std::lock_guard<std::mutex> lock(jobMutex_);
    job->status = "completed";
    job->stage = "completed";
    job->progress = 100;
    job->speed = 0;
    job->message = "已安装官方 llama.cpp " + tag + "（" + backendLabel(target->backend) + "）";
    job->finishedAt = nowMs();
    Logger::instance().info("[llama-runtime] " + job->message);
}

void LlamaRuntimeService::applySettings(const fs::path& releaseDir, const std::string& tag,
                                        const std::string& assetName, const std::string& backend,
                                        const std::string& sourceRoot, bool& changed) {
    json patch = json::object();
    const std::string quantize = findTool(releaseDir, {"llama-quantize.exe", "llama-quantize"}, 2);
    const std::string cli = findTool(releaseDir, {"llama-cli.exe", "llama-cli", "llama.exe", "llama"}, 2);
    const std::string server = findTool(releaseDir, {"llama-server.exe", "llama-server"}, 2);
    const std::string bench = findTool(releaseDir, {"llama-bench.exe", "llama-bench"}, 2);
    const std::string ppl = findTool(releaseDir, {"llama-perplexity.exe", "llama-perplexity"}, 2);
    if (!quantize.empty()) patch["quantizePath"] = quantize;
    if (!cli.empty()) patch["llamaCliPath"] = cli;
    if (!server.empty()) patch["llamaServerPath"] = server;
    if (!bench.empty()) patch["llamaBenchPath"] = bench;
    if (!ppl.empty()) patch["llamaPplPath"] = ppl;
    if (!sourceRoot.empty()) {
        const std::string convert = findTool(pathFromUtf8(sourceRoot), {"convert_hf_to_gguf.py"}, 2);
        if (!convert.empty()) patch["convertScript"] = convert;
    }
    patch["llamaRoot"] = pathToUtf8(root());
    if (!backend.empty()) patch["llamaBackend"] = backend;
    if (patch.empty()) return;
    std::string error;
    settings_.update(patch, error);
    changed = error.empty();
    (void)tag;
    (void)assetName;
}

} // namespace mf
