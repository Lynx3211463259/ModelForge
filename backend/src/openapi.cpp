#include "modelforge/openapi.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace mf {

namespace {

constexpr const char* kApiPrefix = "/api/v1";

// ---------------------------------------------------------------- path helpers

std::string paramNameFor(const std::vector<std::string>& segments, size_t index) {
    for (size_t i = index; i-- > 0;) {
        const std::string& previous = segments[i];
        if (previous == "models") return "modelId";
        if (previous == "conversions") return "taskId";
        if (previous == "downloads") return "downloadId";
        if (previous == "workflows") return "workflowId";
        if (previous == "profiles") return "profileId";
        if (previous == "releases") return "tag";
        if (previous == "nodes") return "nodeId";
        if (previous == "events") return "eventId";
    }
    return "id";
}

// ---------------------------------------------------------------- metadata

struct Doc {
    const char* tag;
    const char* summary;
    const char* body;      // component name for the request body, may be null
    const char* ok;        // component name for a successful payload, may be null
    const char* params;    // comma separated query parameters
    int okStatus;
};

using DocMap = std::map<std::string, Doc>;

const DocMap& docs() {
    static const DocMap table = {
        {"get /api/v1/openapi.json", {"system", "OpenAPI document for this server", nullptr, "OpenApiDocument", nullptr, 200}},
        {"get /api/v1/health", {"system", "Health probe", nullptr, "HealthReport", nullptr, 200}},
        {"get /api/v1/system/summary", {"system", "Workspace summary counters", nullptr, "WorkspaceSummary", nullptr, 200}},
        {"get /api/v1/system/resources", {"system", "Host CPU, memory, disk and GPU snapshot", nullptr, "ResourceSnapshot", nullptr, 200}},
        {"get /api/v1/system/realtime", {"system", "Realtime counters used by the dashboard", nullptr, "RealtimeSnapshot", nullptr, 200}},
        {"get /api/v1/capabilities", {"system", "Conversion engine capabilities and issues", nullptr, "CapabilityReport", nullptr, 200}},
        {"get /api/v1/history", {"system", "Conversion history timeline", nullptr, "HistoryEntryList", "limit", 200}},
        {"get /api/v1/events", {"system", "Recent realtime events (polling fallback)", nullptr, "EventList", "limit", 200}},

        {"get /api/v1/models", {"models", "List registered models", nullptr, "ModelList", "status,favorite,q", 200}},
        {"post /api/v1/models/import", {"models", "Import one or more model directories", "ModelImportRequest", "ModelImportResult", nullptr, 200}},
        {"get /api/v1/models/{modelId}", {"models", "Get one model with files and versions", nullptr, "Model", nullptr, 200}},
        {"patch /api/v1/models/{modelId}", {"models", "Update model metadata (notes, tags, favorite)", "ModelUpdateRequest", "Model", nullptr, 200}},
        {"delete /api/v1/models/{modelId}", {"models", "Delete a model record (files are kept)", nullptr, "DeleteResult", nullptr, 200}},
        {"post /api/v1/models/{modelId}/analyze", {"models", "Re-scan the model directory", nullptr, "Model", nullptr, 200}},
        {"post /api/v1/models/{modelId}/validate", {"models", "Validate model completeness", nullptr, "ValidationReport", nullptr, 200}},
        {"get /api/v1/models/{modelId}/estimates", {"models", "Size and memory estimates per quantization", nullptr, "EstimateReport", nullptr, 200}},
        {"post /api/v1/models/{modelId}/recommend", {"models", "Quantization recommendation for a model", "RecommendRequest", "Recommendation", nullptr, 200}},
        {"get /api/v1/models/{modelId}/versions", {"models", "Version tree: source model plus every produced GGUF", nullptr, "VersionTree", nullptr, 200}},

        {"get /api/v1/conversions", {"conversions", "List conversion tasks", nullptr, "TaskList", "status,modelId,limit", 200}},
        {"post /api/v1/conversions", {"conversions", "Create a conversion task", "ConversionRequest", "Task", nullptr, 202}},
        {"post /api/v1/conversions/preview", {"conversions", "Preview the exact command line without running it", "ConversionRequest", "CommandPreview", nullptr, 200}},
        {"get /api/v1/conversions/{taskId}", {"conversions", "Get one task", nullptr, "Task", nullptr, 200}},
        {"patch /api/v1/conversions/{taskId}", {"conversions", "Update task priority or notes", "TaskUpdateRequest", "Task", nullptr, 200}},
        {"delete /api/v1/conversions/{taskId}", {"conversions", "Delete a task record (outputs are kept)", nullptr, "DeleteResult", nullptr, 200}},
        {"post /api/v1/conversions/{taskId}/cancel", {"conversions", "Cancel a running or queued task", nullptr, "Task", nullptr, 200}},
        {"post /api/v1/conversions/{taskId}/pause", {"conversions", "Pause a queued task", nullptr, "Task", nullptr, 200}},
        {"post /api/v1/conversions/{taskId}/resume", {"conversions", "Resume a paused task", nullptr, "Task", nullptr, 200}},
        {"post /api/v1/conversions/{taskId}/retry", {"conversions", "Retry a failed task with the same parameters", nullptr, "Task", nullptr, 200}},
        {"get /api/v1/conversions/{taskId}/logs", {"conversions", "Structured task logs", nullptr, "LogPage", "afterSeq,limit,level,q", 200}},
        {"get /api/v1/conversions/{taskId}/logs/download", {"conversions", "Download the raw JSONL log file", nullptr, nullptr, nullptr, 200}},
        {"get /api/v1/conversions/{taskId}/result", {"conversions", "Task result summary", nullptr, "TaskResult", nullptr, 200}},
        {"get /api/v1/conversions/{taskId}/resources", {"conversions", "Per-process CPU, memory and IO counters", nullptr, "TaskResources", nullptr, 200}},
        {"get /api/v1/conversions/{taskId}/report", {"conversions", "Verification report JSON", nullptr, "VerificationReport", nullptr, 200}},
        {"get /api/v1/conversions/{taskId}/download", {"conversions", "Download the produced GGUF", nullptr, nullptr, nullptr, 200}},

        {"get /api/v1/queue", {"queue", "Queue statistics", nullptr, "QueueStats", nullptr, 200}},
        {"put /api/v1/queue/concurrency", {"queue", "Change the maximum concurrent tasks", "ConcurrencyRequest", "QueueStats", nullptr, 200}},

        {"get /api/v1/quantization/profiles", {"quantization", "List built-in and custom quantization presets", nullptr, "QuantProfileList", nullptr, 200}},
        {"post /api/v1/quantization/profiles", {"quantization", "Create a quantization preset", "QuantProfileRequest", "QuantProfile", nullptr, 201}},
        {"put /api/v1/quantization/profiles/{profileId}", {"quantization", "Update a custom preset", "QuantProfileRequest", "QuantProfile", nullptr, 200}},
        {"delete /api/v1/quantization/profiles/{profileId}", {"quantization", "Delete a custom preset", nullptr, "DeleteResult", nullptr, 200}},
        {"post /api/v1/quantization/recommend", {"quantization", "Recommend quantization for a model and memory budget", "RecommendRequest", "Recommendation", nullptr, 200}},
        {"get /api/v1/benchmarks", {"benchmarks", "List benchmark runs", nullptr, "BenchmarkList", "limit", 200}},
        {"post /api/v1/benchmarks/run", {"benchmarks", "Run llama-bench for a produced GGUF", "BenchmarkRequest", "BenchmarkResult", nullptr, 200}},
        {"post /api/v1/tester/run", {"tester", "Run one-shot llama-cli inference", "TesterRequest", "TesterResult", nullptr, 200}},

        {"post /api/v1/gguf/inspect", {"gguf", "Inspect GGUF header, metadata and tensors", "GgufInspectRequest", "GgufInfo", nullptr, 200}},
        {"patch /api/v1/gguf/metadata", {"gguf", "Write GGUF metadata (not available in this build)", "GgufMetadataUpdate", nullptr, nullptr, 501}},
        {"get /api/v1/plugins/engines", {"plugins", "Conversion engine adapters and plugin manifests", nullptr, "EngineList", nullptr, 200}},
        {"get /api/v1/nodes", {"nodes", "Remote execution nodes (not enabled)", nullptr, "NodeList", nullptr, 200}},

        {"get /api/v1/workflows", {"workflows", "List workflows", nullptr, "WorkflowList", nullptr, 200}},
        {"post /api/v1/workflows", {"workflows", "Create a workflow", "WorkflowRequest", "Workflow", nullptr, 201}},
        {"put /api/v1/workflows/{workflowId}", {"workflows", "Update a workflow", "WorkflowRequest", "Workflow", nullptr, 200}},
        {"delete /api/v1/workflows/{workflowId}", {"workflows", "Delete a workflow", nullptr, "DeleteResult", nullptr, 200}},
        {"post /api/v1/workflows/{workflowId}/run", {"workflows", "Queue every quantization step of a workflow", "WorkflowRunRequest", "WorkflowRunResult", nullptr, 207}},

        {"get /api/v1/settings", {"settings", "Read application settings", nullptr, "Settings", nullptr, 200}},
        {"put /api/v1/settings", {"settings", "Update application settings", "SettingsPatch", "SettingsUpdateResult", nullptr, 200}},
        {"get /api/v1/settings/export", {"settings", "Export settings as a portable document", nullptr, "SettingsExport", nullptr, 200}},
        {"post /api/v1/settings/import", {"settings", "Import a settings document", "SettingsExport", "Settings", nullptr, 200}},
        {"post /api/v1/settings/test", {"settings", "Run the toolchain checks", nullptr, "ToolchainCheckResult", nullptr, 200}},
        {"get /api/v1/settings/logs", {"settings", "Read application log lines", nullptr, "LogPage", "limit,level,logs", 200}},

        {"get /api/v1/downloads", {"downloads", "List download tasks", nullptr, "DownloadTaskList", "limit,status", 200}},
        {"get /api/v1/downloads/settings", {"downloads", "Download concurrency and rate limit", nullptr, "DownloadSettings", nullptr, 200}},
        {"put /api/v1/downloads/settings", {"downloads", "Update download concurrency and rate limit", "DownloadSettings", "DownloadSettings", nullptr, 200}},
        {"post /api/v1/downloads", {"downloads", "Create a download task", "DownloadCreateRequest", "DownloadTask", nullptr, 202}},
        {"get /api/v1/downloads/{downloadId}", {"downloads", "Get one download task", nullptr, "DownloadTask", nullptr, 200}},
        {"get /api/v1/downloads/{downloadId}/files", {"downloads", "Per-file progress of a download task", nullptr, "DownloadFileList", nullptr, 200}},
        {"get /api/v1/downloads/{downloadId}/logs", {"downloads", "Download task logs", nullptr, "LogPage", "afterSeq,limit", 200}},
        {"post /api/v1/downloads/{downloadId}/pause", {"downloads", "Pause a download task", nullptr, "DownloadTask", nullptr, 200}},
        {"post /api/v1/downloads/{downloadId}/resume", {"downloads", "Resume a download task", nullptr, "DownloadTask", nullptr, 200}},
        {"post /api/v1/downloads/{downloadId}/cancel", {"downloads", "Cancel a download task", nullptr, "DownloadTask", nullptr, 200}},
        {"post /api/v1/downloads/{downloadId}/retry", {"downloads", "Retry failed files of a download task", nullptr, "DownloadTask", nullptr, 200}},
        {"delete /api/v1/downloads/{downloadId}", {"downloads", "Delete a download task record", nullptr, "DeleteResult", nullptr, 200}},

        {"post /api/v1/huggingface/resolve", {"huggingface", "Resolve a repository and list files", "HfResolveRequest", "HfRepository", nullptr, 200}},
        {"get /api/v1/huggingface/repository", {"huggingface", "Resolve a repository by query parameter", nullptr, "HfRepository", "repo,revision", 200}},
        {"get /api/v1/huggingface/files", {"huggingface", "List repository files with sizes and digests", nullptr, "HfFileList", "repo,revision", 200}},
        {"get /api/v1/huggingface/token", {"huggingface", "Token status (never returns the token)", nullptr, "HfTokenStatus", nullptr, 200}},
        {"post /api/v1/huggingface/token/validate", {"huggingface", "Validate a token against the Hugging Face API", "HfTokenRequest", "HfTokenValidation", nullptr, 200}},
        {"put /api/v1/huggingface/token", {"huggingface", "Store or clear the token (DPAPI encrypted)", "HfTokenRequest", "HfTokenStatus", nullptr, 200}},

        {"get /api/v1/llama/status", {"runtime", "Local llama.cpp runtime status and job", nullptr, "LlamaStatusReport", nullptr, 200}},
        {"get /api/v1/llama/probe", {"runtime", "Hardware probe and backend preference chain", nullptr, "LlamaProbe", nullptr, 200}},
        {"post /api/v1/llama/update/check", {"runtime", "Check the official release and compare versions", "LlamaCheckRequest", "LlamaCheckResult", nullptr, 200}},
        {"get /api/v1/llama/releases/{tag}/assets", {"runtime", "Official assets of one release with compatibility flags", nullptr, "LlamaRelease", nullptr, 200}},
        {"post /api/v1/llama/install", {"runtime", "Download, verify and extract an official asset", "LlamaInstallRequest", "LlamaInstallJob", nullptr, 202}},
        {"get /api/v1/llama/install/status", {"runtime", "Install job progress and refreshed runtime status", nullptr, "LlamaInstallStatus", nullptr, 200}},
        {"post /api/v1/llama/install/cancel", {"runtime", "Cancel the running install job", nullptr, "LlamaInstallJob", nullptr, 200}},
        {"post /api/v1/llama/refresh", {"runtime", "Re-detect tool paths from disk", nullptr, "LlamaRuntime", nullptr, 200}},
        {"get /api/v1/llama/backends", {"runtime", "Co-installed llama.cpp backends and the active one", nullptr, "LlamaBackendList", nullptr, 200}},
        {"get /api/v1/updates/startup", {"system", "Startup check for ModelForge and llama.cpp updates", nullptr, nullptr, "force", 200}},
        {"post /api/v1/llama/backends/refresh", {"runtime", "Rebuild the backend registry from the releases folder", nullptr, "LlamaBackendList", nullptr, 200}},
        {"post /api/v1/llama/backends/{id}/activate", {"runtime", "Activate a backend and repoint tool paths", nullptr, nullptr, nullptr, 200}},
        {"delete /api/v1/llama/backends/{id}", {"runtime", "Delete an inactive backend directory", nullptr, nullptr, nullptr, 200}},
        {"get /api/v1/library", {"library", "List indexed offline resources", nullptr, nullptr, "type,q,limit", 200}},
        {"get /api/v1/library/stats", {"library", "Totals, per-type sizes and missing resources", nullptr, nullptr, nullptr, 200}},
        {"post /api/v1/library/rescan", {"library", "Re-index models, datasets and backends", nullptr, nullptr, nullptr, 200}},
        {"post /api/v1/library/export", {"library", "Export an offline bundle manifest with optional file copy", nullptr, nullptr, nullptr, 200}},
        {"post /api/v1/library/import", {"library", "Import an offline bundle manifest", nullptr, nullptr, nullptr, 200}},
        {"get /api/v1/library/bundles", {"library", "Export/import bundle history", nullptr, nullptr, "limit", 200}},
        {"post /api/v1/library/verify", {"library", "Verify registered resources still exist", nullptr, nullptr, nullptr, 200}},
        {"delete /api/v1/library/{id}", {"library", "Unregister a resource (files are kept)", nullptr, nullptr, nullptr, 200}},
        {"get /api/v1/health/check", {"health", "Run the dependency self-check", nullptr, nullptr, "deep,network", 200}},
        {"post /api/v1/health/check", {"health", "Run the dependency self-check with options", nullptr, nullptr, nullptr, 200}},
        {"get /api/v1/health/summary", {"health", "Cached health status for dashboards", nullptr, nullptr, nullptr, 200}},
        {"post /api/v1/health/fix", {"health", "Apply a whitelisted repair action", nullptr, nullptr, nullptr, 200}},
        {"get /api/v1/quality/dataset", {"quality", "Perplexity corpus that will be used", nullptr, "QualityDataset", nullptr, 200}},
        {"get /api/v1/quality/ppl", {"quality", "List perplexity results", nullptr, "PplResultList", "limit", 200}},
        {"get /api/v1/quality/ppl/status", {"quality", "Perplexity job status, progress and logs", nullptr, "PplStatus", nullptr, 200}},
        {"post /api/v1/quality/ppl/run", {"quality", "Run llama-perplexity on produced GGUF artifacts", "PplRunRequest", "PplStatus", nullptr, 202}},
        {"post /api/v1/quality/ppl/cancel", {"quality", "Cancel the running perplexity job", nullptr, "PplStatus", nullptr, 200}},
        {"delete /api/v1/quality/ppl/{id}", {"quality", "Delete one perplexity result", nullptr, "DeleteResult", nullptr, 200}},
        {"get /api/v1/quality/compare", {"quality", "Perplexity, size and throughput per artifact", nullptr, "QualityComparison", "modelId", 200}},

        {"get /api/v1/watch/status", {"watch", "Folder watcher status, folders and recent events", nullptr, "WatchStatus", nullptr, 200}},
        {"get /api/v1/watch/events", {"watch", "Folder watcher event log", nullptr, "WatchEventList", "limit", 200}},
        {"post /api/v1/watch/scan", {"watch", "Scan watched folders now", "WatchScanRequest", "WatchScanResult", nullptr, 200}},
    };
    return table;
}

// ---------------------------------------------------------------- schemas

json schemas() {
    auto obj = [](std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required = {}) {
        json properties = json::object();
        for (const auto& item : props) properties[item.first] = item.second;
        json schema{{"type", "object"}, {"properties", properties}};
        if (required.size() > 0) {
            json list = json::array();
            for (const char* name : required) list.push_back(name);
            schema["required"] = list;
        }
        return schema;
    };
    auto ref = [](const char* name) { return json{{"$ref", std::string("#/components/schemas/") + name}}; };
    auto arrayOf = [&](const char* name) { return json{{"type", "array"}, {"items", ref(name)}}; };
    auto str = []() { return json{{"type", "string"}}; };
    auto integer = []() { return json{{"type", "integer"}}; };
    auto number = []() { return json{{"type", "number"}}; };
    auto boolean = []() { return json{{"type", "boolean"}}; };
    auto strings = []() { return json{{"type", "array"}, {"items", json{{"type", "string"}}}}; };

    json result = json::object();
    result["ApiError"] = obj({{"code", str()}, {"message", str()}, {"suggestion", str()}, {"details", json{{"type", "object"}}}},
                             {"code", "message"});
    result["Envelope"] = obj({{"ok", boolean()}, {"data", json{{"type", "object"}}}, {"error", ref("ApiError")}}, {"ok"});

    result["Model"] = obj({{"id", str()}, {"name", str()}, {"path", str()}, {"architecture", str()},
                           {"architectureFamily", str()}, {"parameterCount", integer()}, {"dtype", str()},
                           {"totalSize", integer()}, {"shardCount", integer()}, {"tensorCount", integer()},
                           {"status", str()}, {"supported", boolean()}, {"favorite", boolean()},
                           {"notes", str()}, {"tags", str()}, {"createdAt", str()}, {"updatedAt", str()},
                           {"analysis", json{{"type", "object"}}}, {"files", arrayOf("ModelFile")},
                           {"versions", arrayOf("ModelVersion")}});
    result["ModelFile"] = obj({{"name", str()}, {"path", str()}, {"size", integer()}, {"kind", str()},
                               {"valid", boolean()}, {"sha256", str()}});
    result["ModelVersion"] = obj({{"id", str()}, {"taskId", str()}, {"quantType", str()}, {"path", str()},
                                  {"size", integer()}, {"sha256", str()}, {"parentId", str()},
                                  {"toolVersion", str()}, {"exists", boolean()}, {"createdAt", str()}});
    result["VersionNode"] = obj({{"id", str()}, {"label", str()}, {"kind", str()}, {"taskId", str()},
                                 {"quantType", str()}, {"path", str()}, {"size", integer()}, {"sha256", str()},
                                 {"toolVersion", str()}, {"exists", boolean()}, {"createdAt", str()},
                                 {"children", arrayOf("VersionNode")}});
    result["VersionTree"] = obj({{"modelId", str()}, {"modelName", str()}, {"root", ref("VersionNode")},
                                 {"nodes", arrayOf("VersionNode")},
                                 {"totals", obj({{"count", integer()}, {"size", integer()},
                                                 {"missing", integer()}})}});
    result["ModelList"] = json{{"type", "array"}, {"items", ref("Model")}};
    result["ModelImportRequest"] = obj({{"paths", strings()}, {"path", str()}}, {"paths"});
    result["ModelImportResult"] = obj({{"models", arrayOf("Model")}, {"errors", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                                       {"scanned", integer()}});
    result["ModelUpdateRequest"] = obj({{"notes", str()}, {"tags", str()}, {"favorite", boolean()}, {"name", str()}});
    result["ValidationReport"] = obj({{"ok", boolean()}, {"checks", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});
    result["EstimateReport"] = obj({{"method", str()}, {"options", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});
    result["RecommendRequest"] = obj({{"modelId", str()}, {"mode", str()}, {"availableMemory", integer()}, {"availableVram", integer()}});
    result["Recommendation"] = obj({{"mode", str()}, {"method", str()}, {"suggestions", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});

    result["Task"] = obj({{"id", str()}, {"modelId", str()}, {"modelName", str()}, {"status", str()},
                          {"stage", str()}, {"stageLabel", str()}, {"stageDetail", str()}, {"progress", integer()},
                          {"progressMode", str()}, {"priority", integer()}, {"inputPath", str()}, {"outputPath", str()},
                          {"outputSize", integer()}, {"createdAt", integer()}, {"startedAt", integer()},
                          {"finishedAt", integer()}, {"elapsedMs", integer()}, {"errorCode", str()},
                          {"errorMessage", str()}, {"suggestion", str()}, {"sha256", str()},
                          {"verification", json{{"type", "object"}}}, {"params", json{{"type", "object"}}}});
    result["TaskList"] = json{{"type", "array"}, {"items", ref("Task")}};
    result["ConversionRequest"] = obj({{"modelId", str()}, {"inputPath", str()}, {"outputDir", str()},
                                       {"outputName", str()}, {"outType", str()}, {"quantType", str()},
                                       {"threads", integer()}, {"overwrite", boolean()}, {"keepIntermediate", boolean()},
                                       {"keepTemp", boolean()}, {"verify", boolean()}, {"computeSha256", boolean()},
                                       {"priority", integer()}}, {"modelId"});
    result["TaskUpdateRequest"] = obj({{"priority", integer()}, {"notes", str()}});
    result["CommandPreview"] = obj({{"convert", str()}, {"quantize", str()}, {"outputs", json{{"type", "object"}}}});
    result["TaskResult"] = obj({{"id", str()}, {"status", str()}, {"outputPath", str()}, {"outputSize", integer()},
                                {"sha256", str()}, {"elapsedMs", integer()}});
    result["VerificationReport"] = obj({{"valid", boolean()}, {"version", integer()}, {"tensors", integer()},
                                        {"checks", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});
    result["LogEntry"] = obj({{"seq", integer()}, {"ts", str()}, {"level", str()}, {"message", str()}, {"source", str()}});
    result["LogPage"] = obj({{"items", arrayOf("LogEntry")}, {"nextSeq", integer()}, {"total", integer()}});

    result["QueueStats"] = obj({{"running", integer()}, {"queued", integer()}, {"paused", integer()},
                                {"completed", integer()}, {"failed", integer()}, {"concurrency", integer()}});
    result["ConcurrencyRequest"] = obj({{"maxConcurrent", integer()}}, {"maxConcurrent"});
    result["QuantProfile"] = obj({{"id", str()}, {"name", str()}, {"description", str()}, {"builtin", boolean()},
                                  {"params", json{{"type", "object"}}}, {"createdAt", str()}});
    result["QuantProfileList"] = json{{"type", "array"}, {"items", ref("QuantProfile")}};
    result["QuantProfileRequest"] = obj({{"name", str()}, {"description", str()}, {"params", json{{"type", "object"}}},
                                         {"outType", str()}, {"quantType", str()}, {"threads", integer()},
                                         {"verify", boolean()}, {"computeSha256", boolean()}}, {"name"});
    result["BenchmarkResult"] = obj({{"id", str()}, {"modelId", str()}, {"quantType", str()},
                                     {"metrics", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} ,
                                     {"environment", json{{"type", "object"}}}, {"createdAt", str()}});
    result["BenchmarkList"] = json{{"type", "array"}, {"items", ref("BenchmarkResult")}};
    result["BenchmarkRequest"] = obj({{"modelId", str()}, {"ggufPath", str()}, {"runs", integer()}, {"threads", integer()}});
    result["TesterRequest"] = obj({{"modelId", str()}, {"ggufPath", str()}, {"prompt", str()},
                                   {"maxTokens", integer()}, {"threads", integer()}, {"gpuLayers", integer()}}, {"prompt"});
    result["TesterResult"] = obj({{"exitCode", integer()}, {"elapsedMs", integer()}, {"output", str()},
                                  {"ok", boolean()}, {"logs", json{{"type", "array"}, {"items", str()}}}});

    result["GgufInspectRequest"] = obj({{"path", str()}, {"taskId", str()}, {"deep", boolean()}});
    result["GgufInfo"] = obj({{"valid", boolean()}, {"version", integer()}, {"architecture", str()},
                              {"quantType", str()}, {"tensorCount", integer()}, {"fileSize", integer()},
                              {"metadata", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} ,
                              {"tensors", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} ,
                              {"errors", strings()}});
    result["GgufMetadataUpdate"] = obj({{"path", str()}, {"key", str()}, {"value", str()}});
    result["EngineList"] = obj({{"engines", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} ,
                                {"plugins", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} ,
                                {"note", str()}});
    result["NodeList"] = json{{"type", "array"}, {"items", json{{"type", "object"}}}};

    result["Workflow"] = obj({{"id", str()}, {"name", str()}, {"description", str()}, {"enabled", boolean()},
                              {"definition", json{{"type", "object"}}}, {"createdAt", str()}, {"updatedAt", str()}});
    result["WorkflowList"] = json{{"type", "array"}, {"items", ref("Workflow")}};
    result["WorkflowRequest"] = obj({{"name", str()}, {"description", str()}, {"enabled", boolean()},
                                     {"definition", json{{"type", "object"}}}}, {"name"});
    result["WorkflowRunRequest"] = obj({{"modelId", str()}, {"outputDir", str()}, {"priority", integer()}});
    result["WorkflowRunResult"] = obj({{"created", arrayOf("Task")}, {"failed", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});

    result["Settings"] = obj({{"pythonPath", str()}, {"convertScript", str()}, {"quantizePath", str()},
                              {"llamaCliPath", str()}, {"llamaServerPath", str()}, {"llamaBenchPath", str()},
                              {"llamaRoot", str()}, {"llamaBackend", str()}, {"llamaChannel", str()}, {"llamaRepo", str()},
                              {"defaultModelDir", str()}, {"defaultOutputDir", str()}, {"tempDir", str()},
                              {"dataDir", str()}, {"webRoot", str()}, {"bindAddress", str()}, {"port", integer()},
                              {"threads", integer()}, {"maxConcurrent", integer()}, {"autoOpenBrowser", boolean()},
                              {"logTimestamps", boolean()}, {"theme", str()}, {"language", str()}, {"updateChannel", str()},
                              {"llamaPplPath", str()}, {"pplDataset", str()},
                              {"folderWatchEnabled", boolean()}, {"watchFolders", strings()},
                              {"watchIntervalSec", integer()}, {"watchAutoConvert", boolean()},
                              {"watchPresetId", str()}});
    result["SettingsPatch"] = json{{"type", "object"}, {"additionalProperties", true}};
    result["SettingsUpdateResult"] = obj({{"settings", ref("Settings")}, {"restartRequired", boolean()}});
    result["SettingsExport"] = obj({{"format", str()}, {"exportedAt", str()}});
    result["ToolchainCheckResult"] = obj({{"checks", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});

    result["DownloadTask"] = obj({{"id", str()}, {"repoId", str()}, {"revision", str()}, {"status", str()},
                                  {"statusLabel", str()}, {"progress", integer()}, {"downloadedBytes", integer()},
                                  {"totalBytes", integer()}, {"speed", number()}, {"destination", str()},
                                  {"errorCode", str()}, {"errorMessage", str()}, {"files", arrayOf("DownloadFile")},
                                  {"result", json{{"type", "object"}}}, {"createdAt", integer()}});
    result["DownloadTaskList"] = json{{"type", "array"}, {"items", ref("DownloadTask")}};
    result["DownloadFile"] = obj({{"path", str()}, {"size", integer()}, {"downloaded", integer()}, {"status", str()},
                                  {"sha256", str()}, {"checksum", str()}, {"error", str()}});
    result["DownloadFileList"] = obj({{"repository", json{{"type", "object"}}}, {"files", arrayOf("DownloadFile")}});
    result["DownloadSettings"] = obj({{"concurrency", integer()}, {"maxSpeed", integer()}, {"defaultDirectory", str()}});
    result["DownloadCreateRequest"] = obj({{"repoId", str()}, {"revision", str()}, {"destination", str()},
                                           {"files", strings()}, {"concurrency", integer()}, {"maxSpeed", integer()},
                                           {"token", str()}}, {"repoId"});

    result["HfResolveRequest"] = obj({{"input", str()}, {"repo", str()}, {"revision", str()}, {"token", str()}});
    result["HfRepository"] = obj({{"repoId", str()}, {"author", str()}, {"name", str()}, {"revision", str()},
                                  {"commitHash", str()}, {"license", str()}, {"gated", boolean()},
                                  {"privateRepo", boolean()}, {"totalSize", integer()}, {"fileCount", integer()},
                                  {"files", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} });
    result["HfFileList"] = obj({{"repoId", str()}, {"revision", str()}, {"files", json{{"type", "array"}, {"items", json{{"type", "object"}}}}}});
    result["HfTokenRequest"] = obj({{"token", str()}});
    result["HfTokenStatus"] = obj({{"configured", boolean()}, {"scheme", str()}, {"username", str()}, {"updatedAt", str()}});
    result["HfTokenValidation"] = obj({{"valid", boolean()}, {"username", str()}, {"organization", str()},
                                       {"code", str()}, {"message", str()}, {"configured", boolean()}});

    result["LlamaRuntime"] = obj({{"root", str()}, {"tag", str()}, {"versionLabel", str()}, {"backend", str()},
                                  {"backendLabel", str()}, {"installed", boolean()}, {"managed", boolean()},
                                  {"detectionSource", str()}, {"releaseDir", str()}, {"asset", str()},
                                  {"convertScript", str()}, {"convertScriptAvailable", boolean()},
                                  {"integrity", json{{"type", "object"}}}, {"tools", json{{"type", "object"}}},
                                  {"issues", strings()}});
    result["LlamaProbe"] = obj({{"platform", str()}, {"arch", str()}, {"nvidia", boolean()}, {"nvidiaDriver", str()},
                                {"cudaVersion", str()}, {"gpuName", str()}, {"vulkan", boolean()},
                                {"chain", strings()}, {"preference", str()}});
    result["LlamaAsset"] = obj({{"name", str()}, {"url", str()}, {"digest", str()}, {"size", integer()},
                                {"backend", str()}, {"variant", str()}, {"arch", str()}, {"dependency", boolean()},
                                {"compatible", boolean()}});
    result["LlamaRelease"] = obj({{"tag", str()}, {"name", str()}, {"publishedAt", str()}, {"htmlUrl", str()},
                                  {"prerelease", boolean()}, {"assets", arrayOf("LlamaAsset")}});
    result["LlamaCheckResult"] = obj({{"status", str()}, {"message", str()}, {"suggestion", str()},
                                      {"remoteTag", str()}, {"stableTag", str()}, {"fallbackReason", str()},
                                      {"local", ref("LlamaRuntime")}, {"release", ref("LlamaRelease")},
                                      {"asset", ref("LlamaAsset")}, {"available", arrayOf("LlamaAsset")},
                                      {"backendUpgrade", boolean()}, {"chain", strings()}, {"checkedAt", str()}});
    result["LlamaCheckRequest"] = obj({{"force", boolean()}});
    result["LlamaInstallRequest"] = obj({{"tag", str()}, {"asset", str()}, {"includeSource", boolean()}});
    result["LlamaInstallJob"] = obj({{"id", str()}, {"status", str()}, {"statusLabel", str()}, {"stage", str()},
                                     {"message", str()}, {"progress", integer()}, {"downloadedBytes", integer()},
                                     {"totalBytes", integer()}, {"speed", number()}, {"tag", str()}, {"asset", str()},
                                     {"backend", str()}, {"errorCode", str()}, {"errorMessage", str()},
                                     {"errorSuggestion", str()}, {"artifacts", json{{"type", "array"}, {"items", json{{"type", "object"}}}}} });
    result["LlamaStatusReport"] = obj({{"runtime", ref("LlamaRuntime")}, {"probe", ref("LlamaProbe")}, {"job", ref("LlamaInstallJob")}});
    result["LlamaInstallStatus"] = obj({{"job", ref("LlamaInstallJob")}, {"runtime", ref("LlamaRuntime")}});

    result["HealthReport"] = obj({{"ok", boolean()}, {"version", str()}, {"uptimeMs", integer()}});
    result["WorkspaceSummary"] = obj({{"modelCount", integer()}, {"totalSize", integer()}, {"successCount", integer()},
                                      {"outputSize", integer()}, {"queue", ref("QueueStats")}});
    result["ResourceSnapshot"] = obj({{"cpu", number()}, {"memory", number()}, {"memoryAvailable", integer()},
                                      {"diskFree", integer()}, {"diskTotal", integer()}, {"threads", integer()},
                                      {"gpu", str()}});
    result["RealtimeSnapshot"] = obj({{"cpu", number()}, {"memory", number()}, {"tasks", json{{"type", "object"}}}});
    result["CapabilityReport"] = obj({{"conversionReady", boolean()}, {"pythonAvailable", boolean()},
                                      {"convertScriptAvailable", boolean()}, {"quantizeAvailable", boolean()},
                                      {"quantTypes", strings()}, {"conversionTypes", strings()},
                                      {"supportedArchitectures", strings()}, {"issues", strings()}});
    result["HistoryEntry"] = obj({{"id", str()}, {"modelId", str()}, {"taskId", str()}, {"event", str()},
                                  {"detail", str()}, {"createdAt", str()}});
    result["HistoryEntryList"] = json{{"type", "array"}, {"items", ref("HistoryEntry")}};
    result["EventList"] = json{{"type", "array"}, {"items", json{{"type", "object"}}}};
    result["DeleteResult"] = obj({{"deleted", boolean()}});
    result["OpenApiDocument"] = json{{"type", "object"}};
    result["TaskResources"] = obj({{"stages", arrayOf("TaskResourceStage")}, {"cpuMs", integer()},
                                    {"durationMs", integer()}, {"peakMemory", integer()},
                                    {"readBytes", integer()}, {"writeBytes", integer()},
                                    {"cpuPercent", number()}, {"sampled", boolean()}});
    result["TaskResourceStage"] = obj({{"stage", str()}, {"stageLabel", str()}, {"cpuMs", integer()},
                                       {"durationMs", integer()}, {"peakMemory", integer()},
                                       {"readBytes", integer()}, {"writeBytes", integer()},
                                       {"samples", integer()}, {"updatedAt", str()}});

    result["QualityDataset"] = obj({{"dataset", obj({{"path", str()}, {"kind", str()}, {"bytes", integer()},
                                                     {"standard", boolean()}, {"exists", boolean()}, {"hint", str()}})},
                                    {"pplPath", str()}, {"pplAvailable", boolean()}});
    result["PplResult"] = obj({{"id", str()}, {"modelId", str()}, {"versionId", str()}, {"quantType", str()},
                               {"path", str()}, {"datasetPath", str()}, {"datasetKind", str()},
                               {"context", integer()}, {"chunks", integer()}, {"tokens", integer()},
                               {"ppl", number()}, {"pplStderr", number()}, {"durationMs", integer()},
                               {"cpuMs", integer()}, {"peakMemory", integer()}, {"createdAt", str()},
                               {"finalLine", str()}});
    result["PplResultList"] = json{{"type", "array"}, {"items", ref("PplResult")}};
    result["PplStatus"] = obj({{"status", str()}, {"progress", integer()}, {"total", integer()},
                               {"completed", integer()}, {"currentTarget", str()}, {"message", str()},
                               {"errorCode", str()}, {"errorMessage", str()},
                               {"results", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                               {"logs", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                               {"dataset", json{{"type", "object"}}}});
    result["PplRunRequest"] = obj({{"modelId", str()}, {"path", str()}, {"quantType", str()},
                                   {"targets", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                                   {"context", integer()}, {"threads", integer()}, {"datasetPath", str()}});
    result["QualityComparison"] = obj({{"modelId", str()}, {"modelName", str()}, {"sourceSize", integer()},
                                       {"rows", arrayOf("QualityRow")}, {"bestPpl", number()},
                                       {"scored", integer()}, {"dataset", json{{"type", "object"}}},
                                       {"caveat", str()}});
    result["QualityRow"] = obj({{"versionId", str()}, {"quantType", str()}, {"path", str()}, {"size", integer()},
                                {"sha256", str()}, {"exists", boolean()}, {"createdAt", str()},
                                {"ppl", json{{"type", "object"}}}, {"benchmark", json{{"type", "object"}}},
                                {"pplDelta", number()}});

    result["WatchStatus"] = obj({{"enabled", boolean()}, {"intervalSec", integer()}, {"autoConvert", boolean()},
                                 {"presetId", str()}, {"folders", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                                 {"lastScanAt", str()}, {"lastScanDurationMs", integer()}, {"nextScanAt", str()},
                                 {"lastError", str()}, {"stats", json{{"type", "object"}}},
                                 {"events", arrayOf("WatchEvent")}});
    result["WatchEvent"] = obj({{"id", str()}, {"folder", str()}, {"path", str()}, {"action", str()},
                                {"modelId", str()}, {"taskId", str()}, {"detail", str()}, {"createdAt", str()}});
    result["WatchEventList"] = json{{"type", "array"}, {"items", ref("WatchEvent")}};
    result["WatchScanRequest"] = obj({{"force", boolean()}});
    result["WatchScanResult"] = obj({{"scannedAt", str()}, {"imported", integer()}, {"skipped", integer()},
                                     {"queued", integer()}, {"failed", integer()},
                                     {"folders", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                                     {"errors", json{{"type", "array"}, {"items", json{{"type", "object"}}}}},
                                     {"durationMs", integer()}});

    return result;
}

std::string camel(const std::string& method, const std::string& path) {
    std::string out = method;
    bool upper = true;
    for (char c : path) {
        if (c == '/') {
            upper = true;
            continue;
        }
        if (c == '{' || c == '}') continue;
        if (upper) {
            out.push_back(static_cast<char>(::toupper(static_cast<unsigned char>(c))));
            upper = false;
        } else if (c == '-') {
            upper = true;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

json queryParameters(const std::string& params) {
    json list = json::array();
    if (params.empty()) return list;
    std::string current;
    std::vector<std::string> names;
    for (char c : params) {
        if (c == ',') {
            names.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) names.push_back(current);
    for (const auto& name : names) {
        const bool numeric = name == "limit" || name == "afterSeq" || name == "offset";
        const bool booleanish = name == "favorite" || name == "deep" || name == "force";
        json schema = numeric ? json{{"type", "integer"}}
                              : booleanish ? json{{"type", "boolean"}}
                                           : json{{"type", "string"}};
        list.push_back(json{{"name", name}, {"in", "query"}, {"required", false}, {"schema", schema}});
    }
    return list;
}

} // namespace

// ---------------------------------------------------------------- registry

void OpenApiRegistry::record(const std::string& method, const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& item : routes_) {
        if (item.value("method", "") == method && item.value("pattern", "") == pattern) return;
    }
    routes_.push_back(json{{"method", method}, {"pattern", pattern}});
}

json OpenApiRegistry::routes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return routes_;
}

std::string OpenApiRegistry::normalizePath(const std::string& pattern) {
    // A pattern group such as "([^/]+)" contains a slash, so the split must respect
    // quotes, brackets and parentheses instead of cutting on every '/'.
    std::vector<std::string> segments;
    std::string current;
    int depth = 0;
    for (char c : pattern) {
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') depth = depth > 0 ? depth - 1 : 0;
        if (c == '/' && depth == 0) {
            segments.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    segments.push_back(current);

    std::set<std::string> used;
    std::string out;
    for (size_t i = 0; i < segments.size(); ++i) {
        const std::string& segment = segments[i];
        const bool isPattern = segment.find('(') != std::string::npos || segment.find('\\') != std::string::npos ||
                               segment.find('[') != std::string::npos;
        if (!isPattern) {
            out += "/" + segment;
            continue;
        }
        std::string name = paramNameFor(segments, i);
        int suffix = 2;
        while (used.count(name)) name = paramNameFor(segments, i) + std::to_string(suffix++);
        used.insert(name);
        out += "/{" + name + "}";
    }
    // The first segment is empty (path starts with '/'), which would yield "//api".
    while (out.find("//") != std::string::npos) out = replaceAll(out, "//", "/");
    return out;
}

// ---------------------------------------------------------------- document

json OpenApiRegistry::build(const json& routes, const std::string& version) {
    json paths = json::object();
    json documented = json::array();
    json undocumented = json::array();
    int documentedCount = 0;

    for (const auto& route : routes) {
        const std::string method = route.value("method", "get");
        const std::string path = normalizePath(route.value("pattern", ""));
        if (path.empty() || path == "/") continue;

        json parameters = json::array();
        // Path parameters from the template itself.
        size_t pos = 0;
        while ((pos = path.find('{', pos)) != std::string::npos) {
            const size_t end = path.find('}', pos);
            if (end == std::string::npos) break;
            const std::string name = path.substr(pos + 1, end - pos - 1);
            parameters.push_back(json{{"name", name}, {"in", "path"}, {"required", true},
                                      {"schema", json{{"type", "string"}}}});
            pos = end + 1;
        }

        const std::string key = method + " " + path;
        const auto it = docs().find(key);
        json operation;
        if (it != docs().end()) {
            ++documentedCount;
            documented.push_back(key);
            const Doc& doc = it->second;
            operation = json{{"tags", json::array({doc.tag})},
                             {"summary", doc.summary},
                             {"operationId", camel(method, path)},
                             {"x-documented", true}};
            const json extra = queryParameters(doc.params ? doc.params : "");
            for (const auto& parameter : extra) parameters.push_back(parameter);
            if (doc.body) {
                operation["requestBody"] = json{{"required", true},
                                                {"content", json{{"application/json",
                                                                  json{{"schema", json{{"$ref", std::string("#/components/schemas/") + doc.body}}}}}}}};
            }
            const int okStatus = doc.okStatus;
            json okContent = json::object();
            if (doc.ok) {
                okContent["application/json"] = json{{"schema", json{{"$ref", std::string("#/components/schemas/") + doc.ok}}}};
            } else {
                okContent["application/octet-stream"] = json{{"schema", json{{"type", "string"}, {"format", "binary"}}}};
            }
            json responses = json::object();
            responses[std::to_string(okStatus)] = json{{"description", "Success"}, {"content", okContent}};
            responses["400"] = json{{"description", "Invalid request"},
                                    {"content", json{{"application/json", json{{"schema", json{{"$ref", "#/components/schemas/ApiError"}}}}}}}};
            if (okStatus < 500) {
                responses["404"] = json{{"description", "Not found"},
                                        {"content", json{{"application/json", json{{"schema", json{{"$ref", "#/components/schemas/ApiError"}}}}}}}};
                responses["409"] = json{{"description", "Conflict with the current state"},
                                        {"content", json{{"application/json", json{{"schema", json{{"$ref", "#/components/schemas/ApiError"}}}}}}}};
            }
            operation["responses"] = responses;
        } else {
            undocumented.push_back(key);
            operation = json{{"tags", json::array({path.find("/llama") != std::string::npos ? "runtime" : "general"})},
                             {"summary", method + " " + path},
                             {"operationId", camel(method, path)},
                             {"x-documented", false},
                             {"responses", json{{"200", json{{"description", "Success"}}}}}};
        }
        if (!parameters.empty()) operation["parameters"] = parameters;
        paths[path][method] = operation;
    }

    json tags = json::array({
        json{{"name", "system"}, {"description", "Health, capabilities, history and realtime"}},
        json{{"name", "models"}, {"description", "Model library, analysis and version tree"}},
        json{{"name", "conversions"}, {"description", "Conversion tasks, logs and artifacts"}},
        json{{"name", "queue"}, {"description", "Scheduler state and concurrency"}},
        json{{"name", "quantization"}, {"description", "Quantization presets and recommendations"}},
        json{{"name", "benchmarks"}, {"description", "llama-bench runs"}},
        json{{"name", "tester"}, {"description", "One-shot llama-cli inference"}},
        json{{"name", "gguf"}, {"description", "GGUF inspection and metadata"}},
        json{{"name", "workflows"}, {"description", "Reusable conversion pipelines"}},
        json{{"name", "settings"}, {"description", "Toolchain, directories and theme"}},
        json{{"name", "downloads"}, {"description", "Hugging Face download tasks"}},
        json{{"name", "huggingface"}, {"description", "Repository resolution and tokens"}},
        json{{"name", "runtime"}, {"description", "Official llama.cpp runtime and updates"}},
        json{{"name", "quality"}, {"description", "Perplexity evaluation and comparisons"}},
        json{{"name", "watch"}, {"description", "Folder watcher and automatic import"}},
        json{{"name", "library"}, {"description", "Offline resource index and bundles"}},
        json{{"name", "health"}, {"description", "Dependency self-check and safe repairs"}},
        json{{"name", "plugins"}, {"description", "Conversion engine adapters"}},
        json{{"name", "nodes"}, {"description", "Remote nodes (disabled)"}},
    });

    return json{
        {"openapi", "3.0.3"},
        {"info", json{{"title", "ModelForge API"},
                      {"version", version},
                      {"description",
                       "Local AI model conversion and management API. Every response uses the "
                       "{ok,data} / {ok,error} envelope. The document is generated from the routes that are "
                       "actually registered in this build, so it cannot drift from the implementation."}}},
        {"servers", json::array({json{{"url", "/api/v1"}}})},
        {"tags", tags},
        {"paths", paths},
        {"components", json{{"schemas", schemas()}}},
        {"x-route-coverage", json{{"registered", routes.size()},
                                  {"documented", documentedCount},
                                  {"undocumented", undocumented},
                                  {"complete", undocumented.empty()}}},
    };
}

} // namespace mf
