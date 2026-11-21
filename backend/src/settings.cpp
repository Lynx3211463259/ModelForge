#include "modelforge/settings.hpp"

#include "modelforge/llama_runtime.hpp"

#include <algorithm>

namespace mf {

namespace {
std::string existingFile(const std::vector<fs::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return pathToUtf8(fs::absolute(candidate));
    }
    return "";
}

std::string existingDir(const std::vector<fs::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) return pathToUtf8(fs::absolute(candidate));
    }
    return "";
}

bool samePath(const fs::path& left, const fs::path& right) {
    std::error_code ec;
    std::string a = pathToUtf8(fs::absolute(left, ec).lexically_normal());
    std::string b = pathToUtf8(fs::absolute(right, ec).lexically_normal());
#ifdef _WIN32
    a = toLower(a);
    b = toLower(b);
#endif
    return a == b;
}
} // namespace

SettingsService::SettingsService(Database& db, const fs::path& root, const fs::path& dataDir,
                                 const fs::path& runtimeRoot)
    : db_(db), root_(fs::absolute(root)),
      runtimeRoot_(fs::absolute(runtimeRoot.empty() ? root : runtimeRoot)), dataOverride_(dataDir) {
    load();
}

AppSettings SettingsService::makeDefaults() const {
    AppSettings s;
    s.dataDir = pathToUtf8(dataOverride_.empty() ? (runtimeRoot_ / "data") : dataOverride_);
    s.defaultModelDir = pathToUtf8(runtimeRoot_ / "models");
    s.defaultOutputDir = pathToUtf8(runtimeRoot_ / "output");
    s.tempDir = pathToUtf8(runtimeRoot_ / "temp");
    s.webRoot = detectWebRoot();
    s.threads = defaultThreadCount();
    const std::string venvPython = existingFile({
        runtimeRoot_ / ".venv" / "Scripts" / "python.exe",
        runtimeRoot_ / ".venv" / "bin" / "python",
        root_ / ".venv" / "Scripts" / "python.exe",
        root_ / ".venv" / "bin" / "python"
    });
    if (!venvPython.empty()) s.pythonPath = venvPython;
    // The distributed llama.cpp is an untouched official release: the binaries live
    // in "<llamaRoot>/releases/<tag-asset>/" and the official tag source archive in
    // "<llamaRoot>/source/llama.cpp-<tag>/". Always prefer those over dev builds.
    const fs::path llamaBase = runtimeRoot_ / "tools" / "llama.cpp";
    s.llamaRoot = pathToUtf8(llamaBase);
    s.convertScript = LlamaRuntimeService::findTool(
        llamaBase, {"convert_hf_to_gguf.py"}, 4);
    if (s.convertScript.empty()) {
        s.convertScript = existingFile({
            runtimeRoot_ / "llama.cpp" / "convert_hf_to_gguf.py",
            root_ / "llama.cpp" / "convert_hf_to_gguf.py",
            root_.parent_path() / "llama.cpp" / "convert_hf_to_gguf.py",
            root_ / "tools" / "llama.cpp" / "convert_hf_to_gguf.py"
        });
    }
    s.quantizePath = detectQuantizePath(llamaBase);
    s.llamaCliPath = detectCliPath(llamaBase);
    s.llamaBenchPath = detectBenchPath(llamaBase);
    s.llamaServerPath = detectServerPath(llamaBase);
    s.llamaPplPath = detectPplPath(llamaBase);
    // A standard perplexity corpus is optional; when the user drops one into the
    // runtime data directory it is picked up automatically.
    s.pplDataset = existingFile({
        (dataOverride_.empty() ? runtimeRoot_ : dataOverride_) / "quality" / "dataset.txt",
        (dataOverride_.empty() ? runtimeRoot_ : dataOverride_) / "datasets" / "wiki.test.raw",
        (dataOverride_.empty() ? runtimeRoot_ : dataOverride_) / "datasets" / "wikitext-2-raw" / "wiki.test.raw"
    });
    return s;
}

std::string SettingsService::detectQuantizePath(const fs::path& llamaBase) const {
    const std::string official =
        LlamaRuntimeService::findTool(llamaBase, {"llama-quantize.exe", "llama-quantize"}, 4);
    if (!official.empty()) return official;
    return existingFile({
        llamaBase / "bin" / "llama-quantize.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-quantize.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-quantize",
        root_ / "llama.cpp" / "build" / "bin" / "Release" / "llama-quantize.exe",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-quantize.exe",
        root_ / "llama.cpp" / "build-modelforge" / "bin" / "llama-quantize.exe",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-quantize",
        root_ / "llama.cpp" / "build-msvc" / "bin" / "Release" / "llama-quantize.exe",
        root_ / "tools" / "llama.cpp" / "build" / "bin" / "llama-quantize.exe",
        root_ / "tools" / "llama.cpp" / "bin" / "llama-quantize.exe"
    });
}

std::string SettingsService::detectCliPath(const fs::path& llamaBase) const {
    const std::string official = LlamaRuntimeService::findTool(
        llamaBase, {"llama-cli.exe", "llama-cli", "llama.exe", "llama"}, 4);
    if (!official.empty()) return official;
    return existingFile({
        llamaBase / "bin" / "llama-cli.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-cli.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-cli",
        root_ / "llama.cpp" / "build" / "bin" / "Release" / "llama-cli.exe",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-cli.exe",
        root_ / "llama.cpp" / "build-modelforge" / "bin" / "llama-cli.exe",
        root_ / "tools" / "llama.cpp" / "bin" / "llama-cli.exe",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-cli",
        root_ / "llama.cpp" / "build-msvc" / "bin" / "Release" / "llama-cli.exe"
    });
}

std::string SettingsService::detectBenchPath(const fs::path& llamaBase) const {
    const std::string official =
        LlamaRuntimeService::findTool(llamaBase, {"llama-bench.exe", "llama-bench"}, 4);
    if (!official.empty()) return official;
    return existingFile({
        llamaBase / "bin" / "llama-bench.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-bench.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-bench",
        root_ / "llama.cpp" / "build" / "bin" / "Release" / "llama-bench.exe",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-bench.exe",
        root_ / "llama.cpp" / "build-modelforge" / "bin" / "llama-bench.exe",
        root_ / "tools" / "llama.cpp" / "bin" / "llama-bench.exe",
        root_ / "llama.cpp" / "build-msvc" / "bin" / "Release" / "llama-bench.exe"
    });
}

std::string SettingsService::detectPplPath(const fs::path& llamaBase) const {
    const std::string official = LlamaRuntimeService::findTool(
        llamaBase, {"llama-perplexity.exe", "llama-perplexity"}, 4);
    if (!official.empty()) return official;
    return existingFile({
        llamaBase / "bin" / "llama-perplexity.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-perplexity.exe",
        root_ / "llama.cpp" / "build" / "bin" / "Release" / "llama-perplexity.exe",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-perplexity.exe",
        root_ / "llama.cpp" / "build-modelforge" / "bin" / "llama-perplexity.exe",
        root_ / "tools" / "llama.cpp" / "bin" / "llama-perplexity.exe"
    });
}

std::string SettingsService::detectServerPath(const fs::path& llamaBase) const {
    const std::string official =
        LlamaRuntimeService::findTool(llamaBase, {"llama-server.exe", "llama-server"}, 4);
    if (!official.empty()) return official;
    return existingFile({
        llamaBase / "bin" / "llama-server.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-server.exe",
        root_ / "llama.cpp" / "build" / "bin" / "llama-server",
        root_ / "llama.cpp" / "build-zig" / "bin" / "llama-server.exe",
        root_ / "llama.cpp" / "build-modelforge" / "bin" / "llama-server.exe",
        root_ / "tools" / "llama.cpp" / "bin" / "llama-server.exe",
        root_ / "llama.cpp" / "build-msvc" / "bin" / "Release" / "llama-server.exe"
    });
}

std::string SettingsService::detectWebRoot() const {
    const fs::path exeDir = pathFromUtf8(getExecutablePath()).parent_path();
    return existingDir({
        root_ / "frontend" / "dist",
        exeDir / "web",
        exeDir / ".." / "share" / "modelforge" / "web",
        root_ / "web"
    });
}

void SettingsService::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = makeDefaults();
    const auto rows = db_.query("SELECT value FROM app_settings WHERE key='settings'");
    if (!rows.empty() && rows[0].contains("value")) {
        try {
            settings_ = AppSettings::fromJson(json::parse(rows[0]["value"].get<std::string>()), settings_);
        } catch (...) {
            // Keep defaults when the settings payload is damaged.
            save();
        }
    } else {
        save();
    }

    // One-time upgrade for installations that previously kept runtime folders in the
    // source/install root. Only exact legacy defaults are rewritten; user-customized
    // paths remain untouched.
    bool changed = false;
    auto remapLegacy = [&](std::string& value, const fs::path& legacy, const fs::path& target) {
        if (value.empty()) return;
        if (samePath(pathFromUtf8(value), legacy)) {
            value = pathToUtf8(target);
            changed = true;
        }
    };
    const fs::path targetData = dataOverride_.empty() ? (runtimeRoot_ / "data") : dataOverride_;
    remapLegacy(settings_.dataDir, root_ / "data", targetData);
    remapLegacy(settings_.defaultModelDir, root_ / "models", runtimeRoot_ / "models");
    remapLegacy(settings_.defaultOutputDir, root_ / "output", runtimeRoot_ / "output");
    remapLegacy(settings_.tempDir, root_ / "temp", runtimeRoot_ / "temp");

    // Tool paths recorded by older installs may point at a self-built llama.cpp tree
    // that no longer exists. Re-detect them from the official release layout instead
    // of failing every conversion with a missing-binary error.
    const AppSettings defaults = makeDefaults();
    std::error_code toolEc;
    auto repairTool = [&](std::string& value, const std::string& fallback) {
        if (fallback.empty()) return;
        if (value.empty() || !fs::is_regular_file(pathFromUtf8(value), toolEc)) {
            if (value != fallback) {
                value = fallback;
                changed = true;
            }
        }
    };
    repairTool(settings_.quantizePath, defaults.quantizePath);
    repairTool(settings_.llamaCliPath, defaults.llamaCliPath);
    repairTool(settings_.llamaServerPath, defaults.llamaServerPath);
    repairTool(settings_.llamaBenchPath, defaults.llamaBenchPath);
    repairTool(settings_.llamaPplPath, defaults.llamaPplPath);
    repairTool(settings_.convertScript, defaults.convertScript);
    if (changed) save();
}
AppSettings SettingsService::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

json SettingsService::update(const json& patch, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    json current = settings_.toJson();
    if (!patch.is_object()) {
        error = "设置更新必须是 JSON 对象";
        return current;
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.key() == "dataDir" || it.key() == "webRoot") continue;
        current[it.key()] = it.value();
    }
    AppSettings candidate = AppSettings::fromJson(current, settings_);
    if (candidate.port < 1024 || candidate.port > 65535) {
        error = "端口必须位于 1024-65535";
        return settings_.toJson();
    }
    if (candidate.bindAddress != "127.0.0.1" && candidate.bindAddress != "localhost") {
        error = "当前版本仅允许监听 127.0.0.1 或 localhost；远程访问需要身份认证与 TLS，尚未开放。";
        return settings_.toJson();
    }
    settings_ = candidate;
    std::string dbError;
    const std::string payload = settings_.toJson().dump();
    if (!db_.exec("CREATE TABLE IF NOT EXISTS app_settings(key TEXT PRIMARY KEY,value TEXT NOT NULL,updated_at TEXT NOT NULL)",
                  dbError)) {
        error = dbError;
        return current;
    }
    db_.query("INSERT INTO app_settings(key,value,updated_at) VALUES('settings',?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at",
              {payload, isoNow()}, &dbError);
    if (!dbError.empty()) error = dbError;
    return settings_.toJson();
}

json SettingsService::exportSettings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json out = settings_.toJson();
    out.erase("dataDir");
    out.erase("webRoot");
    out["exportedAt"] = isoNow();
    out["format"] = "modelforge-settings-v1";
    return out;
}

bool SettingsService::importSettings(const json& value, std::string& error) {
    if (!value.is_object() || !value.contains("format") || value["format"] != "modelforge-settings-v1") {
        error = "设置文件格式不受支持";
        return false;
    }
    json patch = value;
    patch.erase("format");
    patch.erase("exportedAt");
    update(patch, error);
    return error.empty();
}

void SettingsService::save() {
    std::string error;
    db_.query("INSERT INTO app_settings(key,value,updated_at) VALUES('settings',?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at",
              {settings_.toJson().dump(), isoNow()}, &error);
}

fs::path SettingsService::root() const { return root_; }
fs::path SettingsService::runtimeRoot() const { return runtimeRoot_; }
fs::path SettingsService::dataDir() const { return pathFromUtf8(settings_.dataDir); }
fs::path SettingsService::logsDir() const { return dataDir() / "logs"; }
fs::path SettingsService::tempDir() const { return pathFromUtf8(settings_.tempDir); }
fs::path SettingsService::outputDir() const { return pathFromUtf8(settings_.defaultOutputDir); }
fs::path SettingsService::modelsDir() const { return pathFromUtf8(settings_.defaultModelDir); }

} // namespace mf
