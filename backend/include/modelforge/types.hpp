#pragma once

#include "json.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mf {

using json = nlohmann::json;

struct ModelScanResult {
    std::string name;
    std::string path;
    std::string architecture;
    std::string architectureFamily;
    std::string dtype;
    uint64_t parameterCount = 0;
    uint64_t totalSize = 0;
    int shardCount = 0;
    int tensorCount = 0;
    int layers = 0;
    int heads = 0;
    int kvHeads = 0;
    int hiddenSize = 0;
    int intermediateSize = 0;
    int vocabSize = 0;
    int contextLength = 0;
    bool complete = false;
    bool supported = false;
    bool tokenizerComplete = false;
    std::vector<std::string> missing;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    json files = json::array();
    json tokenizer = json::object();
    json config = json::object();
    json report = json::object();
};

struct AppSettings {
    std::string pythonPath = "python";
    std::string convertScript;
    std::string quantizePath;
    std::string llamaCliPath;
    std::string llamaServerPath;
    std::string llamaBenchPath;
    std::string llamaPplPath;
    // Directory that holds the official llama.cpp release artifacts.
    std::string llamaRoot;
    // auto | cpu | cuda | vulkan | rocm | sycl | openvino | opencl
    std::string llamaBackend = "auto";
    // auto | stable | nightly
    std::string llamaChannel = "auto";
    std::string llamaRepo = "ggml-org/llama.cpp";
    std::string defaultModelDir;
    // Root folder for Hugging Face datasets (kept apart from model weights).
    std::string datasetsDir;
    std::string defaultOutputDir;
    std::string tempDir;
    std::string dataDir;
    std::string webRoot;
    std::string bindAddress = "127.0.0.1";
    int port = 8080;
    int threads = 4;
    int maxConcurrent = 1;
    bool autoOpenBrowser = true;
    bool logTimestamps = true;
    std::string theme = "dark";
    std::string language = "zh-CN";
    // 应用自身更新通道：stable 仅正式版，nightly 优先预发布/每日构建，
    // auto 直接采用版本号最高的构建（含预发布）。
    std::string updateChannel = "stable";
    // 启动时静默检查 ModelForge 与 llama.cpp 更新；失败/离线不影响启动。
    bool autoCheckUpdates = true;
    bool folderWatchEnabled = false;
    std::vector<std::string> watchFolders;
    int watchIntervalSec = 30;
    bool watchAutoConvert = false;
    std::string watchPresetId;
    // Optional perplexity corpus; empty means "use the built-in sample" when the
    // standard wikitext-2 test file is not present either.
    std::string pplDataset;
    json toJson() const;
    static AppSettings fromJson(const json& value, const AppSettings& defaults);
};

struct EngineCapabilities {
    bool pythonAvailable = false;
    bool pythonDependenciesReady = false;
    bool convertScriptAvailable = false;
    bool quantizeAvailable = false;
    bool cliAvailable = false;
    bool serverAvailable = false;
    bool benchAvailable = false;
    bool pplAvailable = false;
    std::string pythonVersion;
    std::string pythonDependencyError;
    std::string convertScriptVersion;
    std::string quantizeVersion;
    std::string quantizePath;
    std::string convertScriptPath;
    std::string cliPath;
    std::string benchPath;
    std::string pplPath;
    std::vector<std::string> quantTypes;
    std::vector<std::string> conversionTypes;
    std::vector<std::string> supportedArchitectures;
    std::vector<std::string> issues;
    json toJson() const;
};

struct ConversionParams {
    std::string modelId;
    std::string inputPath;
    std::string outputDir;
    std::string outputName;
    std::string outType = "f16";
    std::string quantType;
    std::string tempDir;
    std::string modelName;
    bool overwrite = false;
    bool keepIntermediate = false;
    bool keepTemp = false;
    bool verify = true;
    bool deepVerify = false;
    bool computeSha256 = true;
    bool useTempFile = false;
    bool noLazy = false;
    int threads = 0;
    int priority = 0;
    json extra = json::object();
    json toJson() const;
    static ConversionParams fromJson(const json& value);
};

struct TaskRecord {
    std::string id;
    std::string modelId;
    std::string modelName;
    std::string status = "QUEUED";
    std::string stage = "QUEUED";
    int progress = 0;
    std::string progressMode = "estimated";
    int priority = 0;
    std::string inputPath;
    std::string outputPath;
    uint64_t outputSize = 0;
    int64_t createdAt = 0;
    int64_t startedAt = 0;
    int64_t finishedAt = 0;
    int64_t elapsedMs = 0;
    std::string errorCode;
    std::string errorMessage;
    std::string sha256;
    std::string logsPath;
    std::string verificationJson;
    ConversionParams params;
    json toJson() const;
};

} // namespace mf
