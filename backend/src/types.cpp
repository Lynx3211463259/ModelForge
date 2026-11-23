#include "modelforge/types.hpp"

#include "modelforge/util.hpp"

#include <algorithm>

namespace mf {

json AppSettings::toJson() const {
    return json{
        {"pythonPath", pythonPath}, {"convertScript", convertScript}, {"quantizePath", quantizePath},
        {"llamaCliPath", llamaCliPath}, {"llamaServerPath", llamaServerPath}, {"llamaBenchPath", llamaBenchPath},
        {"llamaPplPath", llamaPplPath}, {"pplDataset", pplDataset},
        {"llamaRoot", llamaRoot}, {"llamaBackend", llamaBackend}, {"llamaChannel", llamaChannel},
        {"llamaRepo", llamaRepo},
        {"defaultModelDir", defaultModelDir}, {"defaultOutputDir", defaultOutputDir}, {"tempDir", tempDir},
        {"dataDir", dataDir}, {"webRoot", webRoot}, {"bindAddress", bindAddress}, {"port", port},
        {"threads", threads}, {"maxConcurrent", maxConcurrent}, {"autoOpenBrowser", autoOpenBrowser},
        {"logTimestamps", logTimestamps}, {"theme", theme}, {"language", language},
        {"updateChannel", updateChannel},
        {"folderWatchEnabled", folderWatchEnabled}, {"watchFolders", watchFolders},
        {"watchIntervalSec", watchIntervalSec}, {"watchAutoConvert", watchAutoConvert},
        {"watchPresetId", watchPresetId}
    };
}

AppSettings AppSettings::fromJson(const json& value, const AppSettings& defaults) {
    AppSettings s = defaults;
    auto str = [&](const char* key, std::string& target) {
        if (value.contains(key) && value[key].is_string()) target = value[key].get<std::string>();
    };
    auto num = [&](const char* key, int& target) {
        if (value.contains(key) && value[key].is_number_integer()) target = value[key].get<int>();
    };
    auto boolean = [&](const char* key, bool& target) {
        if (value.contains(key) && value[key].is_boolean()) target = value[key].get<bool>();
    };
    str("pythonPath", s.pythonPath); str("convertScript", s.convertScript); str("quantizePath", s.quantizePath);
    str("llamaCliPath", s.llamaCliPath); str("llamaServerPath", s.llamaServerPath); str("llamaBenchPath", s.llamaBenchPath);
    str("llamaPplPath", s.llamaPplPath); str("pplDataset", s.pplDataset);
    str("watchPresetId", s.watchPresetId);
    str("llamaRoot", s.llamaRoot); str("llamaBackend", s.llamaBackend); str("llamaChannel", s.llamaChannel);
    str("llamaRepo", s.llamaRepo);
    str("defaultModelDir", s.defaultModelDir); str("defaultOutputDir", s.defaultOutputDir); str("tempDir", s.tempDir);
    str("webRoot", s.webRoot); str("bindAddress", s.bindAddress); str("theme", s.theme); str("language", s.language);
    str("updateChannel", s.updateChannel);
    num("port", s.port); num("threads", s.threads); num("maxConcurrent", s.maxConcurrent);
    boolean("autoOpenBrowser", s.autoOpenBrowser); boolean("logTimestamps", s.logTimestamps);
    boolean("folderWatchEnabled", s.folderWatchEnabled);
    boolean("watchAutoConvert", s.watchAutoConvert);
    num("watchIntervalSec", s.watchIntervalSec);
    if (value.contains("watchFolders") && value["watchFolders"].is_array()) {
        s.watchFolders.clear();
        for (const auto& item : value["watchFolders"]) if (item.is_string()) s.watchFolders.push_back(item.get<std::string>());
    }
    if (s.bindAddress != "127.0.0.1" && s.bindAddress != "localhost") s.bindAddress = "127.0.0.1";
    static const std::vector<std::string> backends{"auto", "cpu", "cuda", "vulkan", "rocm", "sycl", "openvino", "opencl"};
    if (std::find(backends.begin(), backends.end(), toLower(s.llamaBackend)) == backends.end()) s.llamaBackend = "auto";
    else s.llamaBackend = toLower(s.llamaBackend);
    if (s.theme != "light" && s.theme != "system") s.theme = "dark";
    if (s.language != "en-US") s.language = "zh-CN";
    const std::string updateChannel = toLower(s.updateChannel);
    s.updateChannel = (updateChannel == "nightly" || updateChannel == "auto") ? updateChannel : "stable";
    const std::string channel = toLower(s.llamaChannel);
    s.llamaChannel = (channel == "stable" || channel == "nightly") ? channel : "auto";
    if (s.llamaRepo.empty()) s.llamaRepo = "ggml-org/llama.cpp";
    if (s.port < 1024 || s.port > 65535) s.port = defaults.port;
    if (s.maxConcurrent < 1) s.maxConcurrent = 1;
    if (s.maxConcurrent > 8) s.maxConcurrent = 8;
    if (s.threads < 0 || s.threads > 512) s.threads = defaults.threads;
    if (s.watchIntervalSec < 5) s.watchIntervalSec = 5;
    if (s.watchIntervalSec > 3600) s.watchIntervalSec = 3600;
    return s;
}

json EngineCapabilities::toJson() const {
    json result{
        {"pythonAvailable", pythonAvailable}, {"pythonDependenciesReady", pythonDependenciesReady},
        {"convertScriptAvailable", convertScriptAvailable}, {"quantizeAvailable", quantizeAvailable},
        {"cliAvailable", cliAvailable}, {"serverAvailable", serverAvailable}, {"benchAvailable", benchAvailable},
        {"pplAvailable", pplAvailable},
        {"pythonVersion", pythonVersion}, {"pythonDependencyError", pythonDependencyError},
        {"convertScriptVersion", convertScriptVersion}, {"quantizeVersion", quantizeVersion},
        {"quantizePath", quantizePath}, {"convertScriptPath", convertScriptPath}, {"cliPath", cliPath},
        {"benchPath", benchPath}, {"pplPath", pplPath}, {"quantTypes", quantTypes}, {"conversionTypes", conversionTypes},
        {"supportedArchitectures", supportedArchitectures}, {"issues", issues}
    };
    const bool ready = convertScriptAvailable && pythonDependenciesReady && quantizeAvailable;
    result["conversionReady"] = ready;
    result["ready"] = ready;
    return result;
}

json ConversionParams::toJson() const {
    json result{
        {"modelId", modelId}, {"inputPath", inputPath}, {"outputDir", outputDir}, {"outputName", outputName},
        {"outType", outType}, {"quantType", quantType}, {"tempDir", tempDir}, {"modelName", modelName},
        {"overwrite", overwrite}, {"keepIntermediate", keepIntermediate}, {"keepTemp", keepTemp},
        {"verify", verify}, {"deepVerify", deepVerify}, {"computeSha256", computeSha256},
        {"useTempFile", useTempFile}, {"noLazy", noLazy}, {"threads", threads}, {"priority", priority},
        {"extra", extra}
    };
    return result;
}

ConversionParams ConversionParams::fromJson(const json& value) {
    ConversionParams p;
    auto str = [&](const char* key, std::string& target) {
        if (value.contains(key) && value[key].is_string()) target = value[key].get<std::string>();
    };
    auto boolean = [&](const char* key, bool& target) {
        if (value.contains(key) && value[key].is_boolean()) target = value[key].get<bool>();
    };
    auto integer = [&](const char* key, int& target) {
        if (value.contains(key) && value[key].is_number_integer()) target = value[key].get<int>();
    };
    str("modelId", p.modelId); str("inputPath", p.inputPath); str("outputDir", p.outputDir);
    str("outputName", p.outputName); str("outType", p.outType); str("quantType", p.quantType);
    str("tempDir", p.tempDir); str("modelName", p.modelName);
    boolean("overwrite", p.overwrite); boolean("keepIntermediate", p.keepIntermediate);
    boolean("keepTemp", p.keepTemp); boolean("verify", p.verify); boolean("deepVerify", p.deepVerify);
    boolean("computeSha256", p.computeSha256); boolean("useTempFile", p.useTempFile); boolean("noLazy", p.noLazy);
    integer("threads", p.threads); integer("priority", p.priority);
    if (value.contains("extra") && value["extra"].is_object()) p.extra = value["extra"];
    return p;
}

json TaskRecord::toJson() const {
    json result{
        {"id", id}, {"modelId", modelId}, {"modelName", modelName}, {"status", status}, {"stage", stage},
        {"progress", progress}, {"progressMode", progressMode}, {"priority", priority},
        {"inputPath", inputPath}, {"outputPath", outputPath}, {"outputSize", outputSize},
        {"createdAt", createdAt}, {"startedAt", startedAt}, {"finishedAt", finishedAt}, {"elapsedMs", elapsedMs},
        {"errorCode", errorCode}, {"errorMessage", errorMessage}, {"sha256", sha256},
        {"logsPath", logsPath}, {"params", params.toJson()}
    };
    if (!verificationJson.empty()) {
        try { result["verification"] = json::parse(verificationJson); } catch (...) {}
    }
    return result;
}

} // namespace mf
