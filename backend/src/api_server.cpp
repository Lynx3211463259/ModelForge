#include "modelforge/api_server.hpp"

#include "modelforge/gguf_reader.hpp"
#include "modelforge/fs_browser.hpp"
#include "modelforge/hf_client.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/model_scanner.hpp"
#include "modelforge/process_runner.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <fstream>
#include <regex>

namespace mf {

namespace {
void ok(httplib::Response& res, json data, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(json{{"ok", true}, {"data", std::move(data)}}.dump(), "application/json; charset=utf-8");
}

void fail(httplib::Response& res, const std::string& code, const std::string& message, int status = 400,
          const std::string& suggestion = "", const json& details = nullptr) {
    json error{{"code", code}, {"message", message}};
    if (!suggestion.empty()) error["suggestion"] = suggestion;
    if (!details.is_null()) error["details"] = details;
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json; charset=utf-8");
}

std::string idParam(const httplib::Request& req) {
    return req.matches.size() > 1 ? req.matches[1].str() : "";
}

std::string queryValue(const httplib::Request& req, const std::string& key, const std::string& fallback = "") {
    return req.has_param(key) ? req.get_param_value(key) : fallback;
}

int intQuery(const httplib::Request& req, const std::string& key, int fallback) {
    if (!req.has_param(key)) return fallback;
    try { return std::stoi(req.get_param_value(key)); } catch (...) { return fallback; }
}

json taskForId(TaskManager& tasks, const std::string& id) {
    return tasks.getTask(id);
}
} // namespace

ApiServer::ApiServer(SettingsService& settings, ConverterService& converter, TaskManager& tasks,
                     SystemService& system, DownloadManager& downloads, LlamaRuntimeService& llama,
                     QualityService& quality, WatchService& watch, EventHub& events, WsServer& ws,
                     UpdateService& update, OfflineLibrary& library, HealthCheckService& health)
    : settings_(settings), converter_(converter), tasks_(tasks), system_(system), downloads_(downloads),
      llama_(llama), quality_(quality), watch_(watch), events_(events), ws_(ws), update_(update),
      library_(library), health_(health), server_(new httplib::Server()),
      routes_(new OpenApiRegistry(*server_)) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::readJson(const httplib::Request& req, json& out, std::string& error) const {
    if (req.body.size() > 4ull * 1024 * 1024) { error = "请求体过大"; return false; }
    if (req.body.empty()) { out = json::object(); return true; }
    try {
        out = json::parse(req.body);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("JSON 解析失败: ") + ex.what();
        return false;
    }
}

json ApiServer::capabilities() const {
    return converter_.capabilities(false).toJson();
}

bool ApiServer::prepare(std::string& error) {
    startedAt_ = nowMs();
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(120, 0);
    server_->set_payload_max_length(128ull * 1024 * 1024);
    server_->set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (startsWith(req.path, "/api/")) {
            if (res.status == 404 && res.body.empty()) {
                fail(res, "NOT_FOUND", "接口不存在: " + req.path, 404);
            } else if (res.body.empty()) {
                fail(res, "REQUEST_FAILED", "请求处理失败", res.status);
            }
            return;
        }
        if (res.status == 404) {
            std::error_code ec;
            const fs::path index = pathFromUtf8(settings_.get().webRoot) / "index.html";
            if (fs::exists(index, ec)) {
                res.status = 200;
                res.set_content(readTextFile(index), "text/html; charset=utf-8");
            }
        }
    });
    server_->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string message = "服务器内部错误";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& ex) { message = ex.what(); }
        catch (...) {}
        Logger::instance().error("Unhandled API exception: " + message);
        fail(res, "INTERNAL_ERROR", message, 500);
    });
    server_->set_logger([](const httplib::Request& req, const httplib::Response& res) {
        Logger::instance().log("HTTP", req.method + " " + req.path + " -> " + std::to_string(res.status) +
            " (" + std::to_string(res.body.size()) + " bytes)");
    });
    registerRoutes();

    const std::string webRoot = settings_.get().webRoot;
    std::error_code ec;
    if (!webRoot.empty() && fs::is_directory(pathFromUtf8(webRoot), ec)) {
        if (!server_->set_mount_point("/", webRoot)) {
            error = "无法挂载前端静态目录: " + webRoot;
            return false;
        }
    }
    return true;
}

void ApiServer::registerRoutes() {
    registerSystemRoutes();
    registerModelRoutes();
    registerConversionRoutes();
    registerLabRoutes();
    registerGgufRoutes();
    registerWorkflowRoutes();
    registerSettingsRoutes();
    registerDownloadRoutes();
    registerHuggingFaceRoutes();
    registerLlamaRoutes();
    registerQualityRoutes();
    registerWatchRoutes();
    registerUpdateRoutes();
    registerOfflineRoutes();
    registerHealthRoutes();

    routes_->Get("/api/v1/openapi.json", [this](const httplib::Request&, httplib::Response& res) {
        // Generated from the routes registered in this build, so the document always
        // matches the implementation. Routes without explicit metadata are still
        // emitted and flagged with "x-documented": false.
        const json spec = OpenApiRegistry::build(routes_->routes(), MODELEFORGE_VERSION);
        res.set_header("Cache-Control", "no-store");
        res.set_content(spec.dump(2), "application/json; charset=utf-8");
    });

    routes_->Get("/", [this](const httplib::Request&, httplib::Response& res) {
        const fs::path index = pathFromUtf8(settings_.get().webRoot) / "index.html";
        std::error_code ec;
        if (fs::exists(index, ec)) {
            res.set_content(readTextFile(index), "text/html; charset=utf-8");
        } else {
            res.status = 503;
            res.set_content("<h1>ModelForge UI 尚未构建</h1><p>请执行 npm install && npm run build。</p>", "text/html; charset=utf-8");
        }
    });
}

void ApiServer::registerSystemRoutes() {
    routes_->Get("/api/v1/health", [this](const httplib::Request&, httplib::Response& res) {
        const int configuredPort = settings_.get().port;
        const bool portFallback = port() != configuredPort;
        ok(res, json{{"status", "ok"}, {"version", MODELEFORGE_VERSION}, {"uptimeMs", nowMs() - startedAt_},
                     {"time", isoNow()}, {"port", port()}, {"configuredPort", configuredPort},
                     {"portFallback", portFallback},
                     {"portFallbackReason", portFallback ? portFallbackReason_ : ""},
                     {"websocketPort", ws_.port()}, {"websocketRunning", ws_.running()}});
    });
    routes_->Get("/api/v1/fs/roots", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, json{{"roots", fs_browse::roots()}});
    });
    routes_->Get("/api/v1/fs/list", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string fileFlag = toLower(queryValue(req, "files", "false"));
        const bool includeFiles = fileFlag == "true" || fileFlag == "1" || fileFlag == "yes";
        const fs_browse::Listing listing = fs_browse::list(queryValue(req, "path"), includeFiles);
        if (!listing.ok) {
            int status = 500;
            if (listing.code == "INVALID_PATH" || listing.code == "NOT_A_DIRECTORY") status = 400;
            else if (listing.code == "DIRECTORY_NOT_FOUND") status = 404;
            else if (listing.code == "DIRECTORY_PERMISSION_DENIED") status = 403;
            fail(res, listing.code, listing.message, status, listing.suggestion,
                 json{{"path", listing.path}, {"parent", listing.parent}});
            return;
        }
        ok(res, listing.toJson());
    });
    routes_->Get("/api/v1/fs/file", [this](const httplib::Request& req, httplib::Response& res) {
        const fs_browse::FileInfo file = fs_browse::describeFile(queryValue(req, "path"));
        if (!file.ok) {
            int status = 500;
            if (file.code == "FILE_REQUIRED" || file.code == "INVALID_PATH" || file.code == "NOT_A_FILE") status = 400;
            else if (file.code == "FILE_NOT_FOUND") status = 404;
            fail(res, file.code, file.message, status, file.suggestion,
                 json{{"path", file.path}, {"name", file.name}});
            return;
        }
        ok(res, file.toJson());
    });
    routes_->Get("/api/v1/system/summary", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, system_.summary());
    });
    routes_->Get("/api/v1/system/resources", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, system_.resources());
    });
    routes_->Get("/api/v1/system/realtime", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, json{{"websocket", ws_.running()}, {"port", ws_.port()}, {"path", "/ws"},
                     {"sse", "/api/v1/events"}, {"pollIntervalMs", 1200}});
    });
    routes_->Get("/api/v1/capabilities", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, converter_.capabilities(req.has_param("refresh")).toJson());
    });
    routes_->Get("/api/v1/history", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, system_.history(intQuery(req, "limit", 100)));
    });
    routes_->Get("/api/v1/events", [this](const httplib::Request&, httplib::Response& res) {
        const uint64_t id = events_.subscribe();
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream", [this, id](size_t, httplib::DataSink& sink) {
            while (sink.is_writable()) {
                const auto items = events_.waitForEvents(id, 15000);
                if (!sink.is_writable()) break;
                std::string payload;
                if (items.empty()) payload = ": heartbeat\n\n";
                else for (const auto& item : items) payload += "data: " + item.dump() + "\n\n";
                if (!sink.write(payload.data(), payload.size())) break;
            }
            events_.unsubscribe(id);
            return true;
        });
    });
}

void ApiServer::registerModelRoutes() {
    routes_->Get("/api/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, system_.listModels(queryValue(req, "q"), queryValue(req, "favorite") == "true", intQuery(req, "limit", 100)));
    });
    routes_->Post("/api/v1/models/import", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        std::vector<std::string> paths;
        if (body.contains("path") && body["path"].is_string()) paths.push_back(body["path"].get<std::string>());
        if (body.contains("paths") && body["paths"].is_array()) {
            for (const auto& item : body["paths"]) if (item.is_string()) paths.push_back(item.get<std::string>());
        }
        if (paths.empty()) { fail(res, "PATH_REQUIRED", "请提供 path 或 paths", 400); return; }
        json models = json::array();
        json errors = json::array();
        for (const auto& path : paths) {
            std::string code, error;
            json model = system_.importModel(path, code, error);
            if (model.is_null()) errors.push_back(json{{"path", path}, {"code", code}, {"message", error}});
            else models.push_back(model);
        }
        ok(res, json{{"models", models}, {"errors", errors}}, errors.empty() ? 200 : 207);
    });
    routes_->Get(R"(/api/v1/models/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        json model = system_.getModel(idParam(req));
        if (model.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
        ok(res, model);
    });
    routes_->Patch(R"(/api/v1/models/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        std::string error;
        if (!system_.updateModel(idParam(req), body, error)) { fail(res, "MODEL_UPDATE_FAILED", error, 404); return; }
        ok(res, system_.getModel(idParam(req)));
    });
    routes_->Delete(R"(/api/v1/models/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        // deleteFiles defaults to false so older clients keep the record-only
        // behaviour; the Web UI always sends true after the user confirms.
        const std::string flag = toLower(queryValue(req, "deleteFiles", "false"));
        const bool deleteFiles = flag == "true" || flag == "1" || flag == "yes";
        const ModelDeleteResult result = system_.deleteModel(idParam(req), deleteFiles);
        if (!result.ok) {
            int status = 500;
            if (result.code == "MODEL_NOT_FOUND") status = 404;
            else if (result.code == "MODEL_IN_USE") status = 409;
            else if (result.code == "PERMISSION_DENIED") status = 403;
            fail(res, result.code, result.message, status, result.suggestion, result.details);
            return;
        }
        ok(res, result.details);
    });
    routes_->Post(R"(/api/v1/models/([^/]+)/analyze)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        json model = system_.reanalyzeModel(idParam(req), error);
        if (model.is_null()) { fail(res, "ANALYZE_FAILED", error, 400); return; }
        ok(res, model);
    });
    routes_->Post(R"(/api/v1/models/([^/]+)/validate)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        json model = system_.reanalyzeModel(idParam(req), error);
        if (model.is_null()) { fail(res, "VALIDATE_FAILED", error, 400); return; }
        ok(res, json{{"model", model}, {"valid", model.value("status", "") == "READY"},
                     {"analysis", model.value("analysis", json::object())}});
    });
    routes_->Get(R"(/api/v1/models/([^/]+)/estimates)", [this](const httplib::Request& req, httplib::Response& res) {
        json model = system_.getModel(idParam(req));
        if (model.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
        ModelScanResult scan = ModelScanner::scan(pathFromUtf8(model.value("path", "")));
        ok(res, converter_.estimates(scan));
    });
    routes_->Post(R"(/api/v1/models/([^/]+)/recommend)", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        json model = system_.getModel(idParam(req));
        if (model.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
        ModelScanResult scan = ModelScanner::scan(pathFromUtf8(model.value("path", "")));
        const std::string mode = body.value("mode", std::string("balanced"));
        const uint64_t memory = body.value("availableMemory", 0ull);
        const uint64_t vram = body.value("availableVram", 0ull);
        ok(res, converter_.recommend(scan, mode, memory, vram));
    });
    routes_->Get(R"(/api/v1/models/([^/]+)/versions)", [this](const httplib::Request& req, httplib::Response& res) {
        const json tree = system_.modelVersions(idParam(req));
        if (tree.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
        ok(res, tree);
    });
}

void ApiServer::registerConversionRoutes() {
    routes_->Get("/api/v1/conversions", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, tasks_.listTasks(queryValue(req, "status"), queryValue(req, "modelId"), intQuery(req, "limit", 100)));
    });
    routes_->Post("/api/v1/conversions", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        json paramsJson = body.contains("params") ? body["params"] : body;
        ConversionParams params = ConversionParams::fromJson(paramsJson);
        if (params.modelId.empty() && body.contains("modelId")) params.modelId = body["modelId"].get<std::string>();
        if (!params.modelId.empty()) {
            json model = system_.getModel(params.modelId);
            if (model.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
            if (params.inputPath.empty()) params.inputPath = model.value("path", "");
            if (params.modelName.empty()) params.modelName = model.value("name", "");
            if (params.outputName.empty()) params.outputName = model.value("name", "");
        }
        if (params.outputDir.empty()) params.outputDir = settings_.get().defaultOutputDir;
        std::string taskId, code, error;
        if (!tasks_.createTask(params, taskId, code, error)) {
            fail(res, code, error, code == "MODEL_NOT_FOUND" ? 404 : 400,
                 code == "OUTPUT_EXISTS" ? "启用覆盖，或修改输出名称" : tasks_.errorSuggestion(code));
            return;
        }
        ok(res, tasks_.getTask(taskId), 202);
    });
    routes_->Post("/api/v1/conversions/preview", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        ConversionParams params = ConversionParams::fromJson(body.contains("params") ? body["params"] : body);
        if (!params.modelId.empty()) {
            json model = system_.getModel(params.modelId);
            if (!model.is_null() && params.inputPath.empty()) params.inputPath = model.value("path", "");
        }
        if (params.outputDir.empty()) params.outputDir = settings_.get().defaultOutputDir;
        if (params.outputName.empty()) params.outputName = "model";
        if (params.threads == 0) params.threads = settings_.get().threads;
        ok(res, converter_.commandPreview(params));
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        json task = taskForId(tasks_, idParam(req));
        if (task.is_null()) { fail(res, "TASK_NOT_FOUND", "任务不存在", 404); return; }
        ok(res, task);
    });
    routes_->Delete(R"(/api/v1/conversions/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!tasks_.removeTask(idParam(req), queryValue(req, "logs") == "true", error)) {
            fail(res, "TASK_DELETE_FAILED", error, 409);
            return;
        }
        ok(res, json{{"deleted", true}});
    });
    routes_->Post(R"(/api/v1/conversions/([^/]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!tasks_.cancelTask(idParam(req), error)) { fail(res, "CANCEL_FAILED", error, 409); return; }
        ok(res, json{{"canceling", true}});
    });
    routes_->Post(R"(/api/v1/conversions/([^/]+)/pause)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!tasks_.pauseTask(idParam(req), error)) { fail(res, "PAUSE_FAILED", error, 409); return; }
        ok(res, json{{"paused", true}});
    });
    routes_->Post(R"(/api/v1/conversions/([^/]+)/resume)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!tasks_.resumeTask(idParam(req), error)) { fail(res, "RESUME_FAILED", error, 409); return; }
        ok(res, json{{"resumed", true}});
    });
    routes_->Post(R"(/api/v1/conversions/([^/]+)/retry)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string newId, error;
        if (!tasks_.retryTask(idParam(req), newId, error)) { fail(res, "RETRY_FAILED", error, 400); return; }
        ok(res, tasks_.getTask(newId), 202);
    });
    routes_->Patch(R"(/api/v1/conversions/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        std::string error;
        if (!tasks_.setPriority(idParam(req), body.value("priority", 0), error)) {
            fail(res, "PRIORITY_FAILED", error, 409);
            return;
        }
        ok(res, tasks_.getTask(idParam(req)));
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+)/logs)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = idParam(req);
        if (tasks_.getTask(id).is_null()) { fail(res, "TASK_NOT_FOUND", "任务不存在", 404); return; }
        ok(res, tasks_.logs(id, intQuery(req, "afterSeq", 0), intQuery(req, "limit", 500), queryValue(req, "level")));
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+)/logs/download)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = idParam(req);
        if (tasks_.getTask(id).is_null()) { fail(res, "TASK_NOT_FOUND", "任务不存在", 404); return; }
        const fs::path path = tasks_.logFilePath(id);
        std::error_code ec;
        if (!fs::exists(path, ec)) { fail(res, "LOG_NOT_FOUND", "日志文件不存在", 404); return; }
        res.set_header("Content-Disposition", "attachment; filename=\"" + id + ".log.jsonl\"");
        res.set_file_content(pathToUtf8(path), "application/x-ndjson");
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+)/resources)", [this](const httplib::Request& req, httplib::Response& res) {
        const json task = tasks_.getTask(idParam(req));
        if (task.is_null()) return fail(res, "TASK_NOT_FOUND", "任务不存在", 404);
        ok(res, tasks_.resources(idParam(req)));
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+)/result)", [this](const httplib::Request& req, httplib::Response& res) {
        json result = tasks_.result(idParam(req));
        if (result.is_null()) { fail(res, "TASK_NOT_FOUND", "任务不存在", 404); return; }
        ok(res, result);
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+)/report)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = idParam(req);
        const fs::path path = settings_.dataDir() / "reports" / (id + ".json");
        std::error_code ec;
        if (!fs::exists(path, ec)) { fail(res, "REPORT_NOT_FOUND", "转换报告尚未生成", 404); return; }
        res.set_header("Content-Disposition", "attachment; filename=\"" + id + "-report.json\"");
        res.set_file_content(pathToUtf8(path), "application/json");
    });
    routes_->Get(R"(/api/v1/conversions/([^/]+)/download)", [this](const httplib::Request& req, httplib::Response& res) {
        json result = tasks_.result(idParam(req));
        if (result.is_null()) { fail(res, "TASK_NOT_FOUND", "任务不存在", 404); return; }
        if (!result.value("ready", false)) { fail(res, "NOT_READY", "任务尚未成功完成", 409, result.value("errorCode", "")); return; }
        const fs::path path = pathFromUtf8(result.value("outputPath", ""));
        std::error_code ec;
        if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
            fail(res, "FILE_NOT_FOUND", "输出文件已不存在", 404);
            return;
        }
        res.set_header("Content-Disposition", "attachment; filename=\"" + path.filename().u8string() + "\"");
        res.set_file_content(pathToUtf8(path), "application/octet-stream");
    });
    routes_->Get("/api/v1/queue", [this](const httplib::Request&, httplib::Response& res) { ok(res, tasks_.queueStats()); });
    routes_->Put("/api/v1/queue/concurrency", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        const int value = body.value("maxConcurrent", settings_.get().maxConcurrent);
        tasks_.setConcurrency(value);
        ok(res, json{{"maxConcurrent", settings_.get().maxConcurrent}});
    });
}

void ApiServer::registerLabRoutes() {
    routes_->Get("/api/v1/quantization/profiles", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, system_.quantizationProfiles());
    });
    routes_->Post("/api/v1/quantization/profiles", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        if (body.value("name", std::string("")).empty()) { fail(res, "NAME_REQUIRED", "预设名称不能为空", 400); return; }
        json profile = system_.saveQuantizationProfile("", body);
        ok(res, profile, 201);
    });
    routes_->Put(R"(/api/v1/quantization/profiles/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        json profile = system_.saveQuantizationProfile(idParam(req), body);
        if (profile.is_null()) { fail(res, "PROFILE_FAILED", "预置配置不可修改或不存在", 409); return; }
        ok(res, profile);
    });
    routes_->Delete(R"(/api/v1/quantization/profiles/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!system_.deleteQuantizationProfile(idParam(req), error)) { fail(res, "PROFILE_DELETE_FAILED", error, 409); return; }
        ok(res, json{{"deleted", true}});
    });
    routes_->Post("/api/v1/quantization/recommend", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        const std::string modelId = body.value("modelId", std::string(""));
        json model = system_.getModel(modelId);
        if (model.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
        ModelScanResult scan = ModelScanner::scan(pathFromUtf8(model.value("path", "")));
        ok(res, converter_.recommend(scan, body.value("mode", std::string("balanced")),
                                     body.value("availableMemory", 0ull), body.value("availableVram", 0ull)));
    });
    routes_->Get("/api/v1/benchmarks", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, system_.benchmarks(intQuery(req, "limit", 50)));
    });
    routes_->Post("/api/v1/benchmarks/run", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        const std::string path = body.value("path", std::string(""));
        std::error_code ec;
        if (path.empty() || !fs::is_regular_file(pathFromUtf8(path), ec)) { fail(res, "FILE_NOT_FOUND", "GGUF 文件不存在", 404); return; }
        const AppSettings s = settings_.get();
        if (s.llamaBenchPath.empty() || !fs::is_regular_file(pathFromUtf8(s.llamaBenchPath), ec)) {
            fail(res, "BENCHMARK_UNAVAILABLE", "llama-bench 不可用，请在设置中配置", 409);
            return;
        }
        ProcessOptions opt;
        opt.executable = s.llamaBenchPath;
        opt.args = {"-m", path, "-p", "512", "-n", "128", "-r", "1", "-o", "json"};
        opt.timeoutMs = 600000;
        std::string output;
        opt.onLine = [&](const std::string& line, bool) { output += line + "\n"; };
        const ProcessResult pr = ProcessRunner::run(opt);
        if (!pr.started || pr.exitCode != 0) {
            fail(res, "BENCHMARK_FAILED", "llama-bench 运行失败，退出码 " + std::to_string(pr.exitCode), 500, "", json{{"output", output}});
            return;
        }
        const size_t begin = output.find('[');
        const size_t end = output.rfind(']');
        json metrics = json::array();
        if (begin != std::string::npos && end != std::string::npos && end > begin) {
            try { metrics = json::parse(output.substr(begin, end - begin + 1)); } catch (...) {}
        }
        ok(res, system_.saveBenchmark(path, body.value("modelId", std::string("")), metrics, output, body.value("quantType", std::string(""))));
    });
    routes_->Post("/api/v1/tester/run", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        const std::string path = body.value("path", std::string(""));
        std::error_code ec;
        if (path.empty() || !fs::is_regular_file(pathFromUtf8(path), ec)) { fail(res, "FILE_NOT_FOUND", "GGUF 文件不存在", 404); return; }
        const AppSettings s = settings_.get();
        if (s.llamaCliPath.empty() || !fs::is_regular_file(pathFromUtf8(s.llamaCliPath), ec)) {
            fail(res, "TESTER_UNAVAILABLE", "llama-cli 不可用，请先构建 llama.cpp 并在设置中配置", 409);
            return;
        }
        std::string prompt = body.value("prompt", std::string("Hello"));
        if (prompt.size() > 20000) { fail(res, "PROMPT_TOO_LONG", "提示词最多 20000 字符", 400); return; }
        const int maxTokens = std::max(1, std::min(body.value("maxTokens", 128), 4096));
        const int context = std::max(128, std::min(body.value("context", 2048), 131072));
        const int gpuLayers = std::max(0, std::min(body.value("gpuLayers", 0), 999));
        const int threads = std::max(1, std::min(body.value("threads", s.threads), 512));
        const double temperature = std::max(0.0, std::min(body.value("temperature", 0.7), 5.0));
        const double topP = std::max(0.0, std::min(body.value("topP", 0.95), 1.0));
        const int topK = std::max(0, std::min(body.value("topK", 40), 1000));
        ProcessOptions opt;
        opt.executable = s.llamaCliPath;
        opt.args = {"-m", path, "-p", prompt, "-n", std::to_string(maxTokens), "-c", std::to_string(context),
                    "-ngl", std::to_string(gpuLayers), "-t", std::to_string(threads),
                    "--temp", std::to_string(temperature), "--top-p", std::to_string(topP),
                    "--top-k", std::to_string(topK), "--no-display-prompt", "--no-warmup", "--simple-io", "-st"};
        opt.timeoutMs = 300000;
        int64_t firstLineAt = 0;
        std::string output;
        opt.onLine = [&](const std::string& line, bool) {
            if (firstLineAt == 0) firstLineAt = nowMs();
            if (output.size() < 2 * 1024 * 1024) output += line + "\n";
        };
        const ProcessResult pr = ProcessRunner::run(opt);
        ok(res, json{{"started", pr.started}, {"exitCode", pr.exitCode}, {"elapsedMs", pr.durationMs},
                     {"firstOutputMs", firstLineAt ? firstLineAt - (nowMs() - pr.durationMs) : 0},
                     {"output", output}, {"note", "输出包含 llama-cli 的运行日志与生成文本，均为真实进程输出。"}});
    });
}

void ApiServer::registerGgufRoutes() {
    routes_->Post("/api/v1/gguf/inspect", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        std::string path = body.value("path", std::string(""));
        if (path.empty() && body.contains("taskId") && body["taskId"].is_string()) {
            json result = tasks_.result(body["taskId"].get<std::string>());
            if (result.is_object()) path = result.value("outputPath", "");
        }
        if (path.empty()) { fail(res, "PATH_REQUIRED", "请提供 GGUF 路径或已完成任务 ID", 400); return; }
        std::error_code ec;
        if (!fs::is_regular_file(pathFromUtf8(path), ec)) { fail(res, "FILE_NOT_FOUND", "GGUF 文件不存在", 404); return; }
        GgufInfo info = GgufReader::read(pathFromUtf8(path), true);
        json data = info.toJson();
        data["path"] = path;
        ok(res, data);
    });
    routes_->Patch("/api/v1/gguf/metadata", [this](const httplib::Request&, httplib::Response& res) {
        fail(res, "GGUF_EDIT_READ_ONLY",
             "当前 C++ 版本提供 GGUF 只读检查。安全编辑需要在保持张量偏移与对齐的前提下重写文件头，本阶段未开放写入，以免损坏模型。",
             501, "使用 gguf-py 或 llama.cpp 的 GGUF 编辑工具完成修改，并在修改前备份。");
    });
    routes_->Get("/api/v1/plugins/engines", [this](const httplib::Request&, httplib::Response& res) {
        const EngineCapabilities caps = converter_.capabilities(false);
        json engines = json::array();
        engines.push_back(json{
            {"id", "llama.cpp"}, {"name", "llama.cpp converter"}, {"kind", "conversion"},
            {"version", caps.convertScriptVersion}, {"installed", caps.convertScriptAvailable},
            {"ready", caps.convertScriptAvailable && caps.pythonDependenciesReady},
            {"capabilities", json{{"architectures", caps.supportedArchitectures.size()}, {"outTypes", caps.conversionTypes}}}
        });
        engines.push_back(json{
            {"id", "llama-quantize"}, {"name", "llama.cpp quantizer"}, {"kind", "quantization"},
            {"version", caps.quantizeVersion}, {"installed", caps.quantizeAvailable},
            {"ready", caps.quantizeAvailable}, {"capabilities", json{{"quantTypes", caps.quantTypes}}}
        });
        json plugins = json::array();
        std::error_code ec;
        const fs::path dir = settings_.root() / "plugins";
        if (fs::is_directory(dir, ec)) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
                try {
                    json manifest = readJsonFile(entry.path());
                    plugins.push_back(json{{"id", manifest.value("id", entry.path().stem().u8string())},
                                           {"name", manifest.value("name", entry.path().stem().u8string())},
                                           {"version", manifest.value("version", "0.0.0")},
                                           {"description", manifest.value("description", "")},
                                           {"enabled", false}, {"status", "manifest-only"}});
                } catch (...) {}
            }
        }
        ok(res, json{{"engines", engines}, {"plugins", plugins},
                     {"note", "转换引擎通过内置适配器管理；外部插件清单可被读取，但当前版本不在进程内加载未知二进制。"}});
    });
    routes_->Get("/api/v1/nodes", [this](const httplib::Request&, httplib::Response& res) {
        json nodes = json::array();
        const AppSettings s = settings_.get();
        nodes.push_back(json{{"id", "local"}, {"name", getHostName()}, {"kind", "local"}, {"status", "ONLINE"},
                             {"bindAddress", s.bindAddress}, {"port", s.port}, {"maxConcurrent", s.maxConcurrent}});
        ok(res, json{{"nodes", nodes}, {"remoteEnabled", false},
                     {"note", "远程节点需身份认证、TLS 与文件权限模型，当前版本默认未开放远程执行。"}});
    });
}

void ApiServer::registerWorkflowRoutes() {
    routes_->Get("/api/v1/workflows", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, system_.listWorkflows());
    });
    routes_->Post("/api/v1/workflows", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        if (body.value("name", std::string("")).empty()) { fail(res, "NAME_REQUIRED", "工作流名称不能为空", 400); return; }
        ok(res, system_.saveWorkflow("", body), 201);
    });
    routes_->Put(R"(/api/v1/workflows/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        json workflow = system_.saveWorkflow(idParam(req), body);
        if (workflow.is_null()) { fail(res, "WORKFLOW_NOT_FOUND", "工作流不存在", 404); return; }
        ok(res, workflow);
    });
    routes_->Delete(R"(/api/v1/workflows/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!system_.deleteWorkflow(idParam(req), error)) { fail(res, "WORKFLOW_DELETE_FAILED", error, 404); return; }
        ok(res, json{{"deleted", true}});
    });
    routes_->Post(R"(/api/v1/workflows/([^/]+)/run)", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        const std::string modelId = body.value("modelId", std::string(""));
        json model = system_.getModel(modelId);
        if (model.is_null()) { fail(res, "MODEL_NOT_FOUND", "模型不存在", 404); return; }
        json workflow = system_.getWorkflow(idParam(req));
        if (workflow.is_null()) { fail(res, "WORKFLOW_NOT_FOUND", "工作流不存在", 404); return; }
        const json definition = workflow.value("definition", json::object());
        std::vector<std::string> quants;
        if (definition.contains("quantTypes") && definition["quantTypes"].is_array()) {
            for (const auto& item : definition["quantTypes"]) if (item.is_string()) quants.push_back(item.get<std::string>());
        }
        if (quants.empty()) { fail(res, "WORKFLOW_EMPTY", "工作流未配置量化类型", 400); return; }
        json created = json::array();
        json errors = json::array();
        for (const auto& quant : quants) {
            ConversionParams params;
            params.modelId = modelId;
            params.inputPath = model.value("path", "");
            params.modelName = model.value("name", "");
            params.outputDir = definition.value("outputDir", settings_.get().defaultOutputDir);
            params.outputName = sanitizeFileName(model.value("name", "model") + "." + quant);
            params.outType = definition.value("outType", std::string("f16"));
            params.quantType = quant;
            params.threads = definition.value("threads", settings_.get().threads);
            params.overwrite = definition.value("overwrite", false);
            params.verify = true;
            params.computeSha256 = definition.value("computeSha256", true);
            std::string taskId, code, error;
            if (tasks_.createTask(params, taskId, code, error)) created.push_back(tasks_.getTask(taskId));
            else errors.push_back(json{{"quantType", quant}, {"code", code}, {"message", error}});
        }
        ok(res, json{{"created", created}, {"errors", errors}}, errors.empty() ? 202 : 207);
    });
}

void ApiServer::registerSettingsRoutes() {
    routes_->Get("/api/v1/settings", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, settings_.get().toJson());
    });
    routes_->Put("/api/v1/settings", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        const int oldPort = settings_.get().port;
        std::string error;
        const json updated = settings_.update(body, error);
        if (!error.empty()) { fail(res, "SETTINGS_INVALID", error, 400); return; }
        ok(res, json{{"settings", updated}, {"restartRequired", oldPort != updated.value("port", oldPort)}});
    });
    routes_->Get("/api/v1/settings/export", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Disposition", "attachment; filename=\"modelforge-settings.json\"");
        res.set_content(settings_.exportSettings().dump(2), "application/json");
    });
    routes_->Post("/api/v1/settings/import", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        std::string error;
        if (!settings_.importSettings(body, error)) { fail(res, "SETTINGS_IMPORT_FAILED", error, 400); return; }
        ok(res, settings_.get().toJson());
    });
    routes_->Post("/api/v1/settings/test", [this](const httplib::Request&, httplib::Response& res) {
        const EngineCapabilities caps = converter_.capabilities(true);
        const AppSettings s = settings_.get();
        std::error_code ec;
        json checks = json::array();
        checks.push_back(json{{"id", "python"}, {"ok", caps.pythonAvailable}, {"detail", caps.pythonVersion.empty() ? "未找到 Python" : caps.pythonVersion}});
        checks.push_back(json{{"id", "python-deps"}, {"ok", caps.pythonDependenciesReady}, {"detail", caps.pythonDependenciesReady ? "torch / numpy / transformers 可导入" : caps.pythonDependencyError}});
        checks.push_back(json{{"id", "convert-script"}, {"ok", caps.convertScriptAvailable}, {"detail", s.convertScript}});
        checks.push_back(json{{"id", "quantize"}, {"ok", caps.quantizeAvailable}, {"detail", s.quantizePath}});
        checks.push_back(json{{"id", "llama-cli"}, {"ok", caps.cliAvailable}, {"detail", s.llamaCliPath.empty() ? "未配置" : s.llamaCliPath}});
        checks.push_back(json{{"id", "output-dir"}, {"ok", directoryWritable(settings_.outputDir())}, {"detail", pathToUtf8(settings_.outputDir())}});
        checks.push_back(json{{"id", "temp-dir"}, {"ok", directoryWritable(settings_.tempDir())}, {"detail", pathToUtf8(settings_.tempDir())}});
        ok(res, json{{"capabilities", caps.toJson()}, {"checks", checks}});
    });
    routes_->Get("/api/v1/settings/logs", [this](const httplib::Request&, httplib::Response& res) {
        json files = json::array();
        std::error_code ec;
        const fs::path dir = settings_.logsDir();
        if (fs::is_directory(dir, ec)) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".log") {
                    files.push_back(json{{"name", entry.path().filename().u8string()}, {"size", fs::file_size(entry.path(), ec)},
                                         {"modified", isoNow()}});
                }
            }
        }
        ok(res, files);
    });
}

void ApiServer::registerDownloadRoutes() {
    routes_->Get("/api/v1/downloads", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, downloads_.list(intQuery(req, "limit", 50)));
    });

    routes_->Get("/api/v1/downloads/settings", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, downloads_.settings());
    });

    routes_->Put("/api/v1/downloads/settings", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        if (!downloads_.updateSettings(body, error)) return fail(res, "INVALID_SETTINGS", error, 400);
        ok(res, downloads_.settings());
    });

    routes_->Post("/api/v1/downloads", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        DownloadRequest request;
        request.repoId = body.value("repoId", body.value("repo", ""));
        request.repoType = HfClient::normalizeRepoType(body.value("repoType", body.value("kind", "model")));
        request.revision = body.value("revision", "main");
        request.commitHash = body.value("commitHash", "");
        request.destination = body.value("destination", body.value("outputDir", ""));
        request.concurrency = body.value("concurrency", 4);
        request.maxSpeed = body.value("maxSpeed", 0ull);
        request.verifyChecksum = body.value("verifyChecksum", true);
        request.autoImport = body.value("autoImport", true);
        request.selection = body;
        json sizes = json::object();
        if (body.contains("files") && body["files"].is_array()) {
            for (const auto& entry : body["files"]) {
                if (entry.is_string()) {
                    request.files.push_back(entry.get<std::string>());
                } else if (entry.is_object()) {
                    const std::string path = entry.value("path", "");
                    if (path.empty()) continue;
                    request.files.push_back(path);
                    sizes[path] = json{{"size", entry.value("size", 0ull)}, {"sha256", entry.value("sha256", "")}};
                }
            }
        }
        if (body.contains("sizes") && body["sizes"].is_object()) {
            for (auto it = body["sizes"].begin(); it != body["sizes"].end(); ++it) sizes[it.key()] = it.value();
        }
        if (body.contains("repository") && body["repository"].is_object()) {
            request.selection["repository"] = body["repository"];
        }
        request.selection["sizes"] = sizes;
        std::string id, code;
        if (!downloads_.createTask(request, id, code, error)) {
            return fail(res, code, error, code == "UNSAFE_PATH" ? 400 : 400,
                        code == "UNSAFE_PATH" ? "仓库文件路径被拒绝，请联系仓库维护者。" : "");
        }
        ok(res, downloads_.getTask(id, true), 201);
    });

    routes_->Get(R"(/api/v1/downloads/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        const json task = downloads_.getTask(idParam(req), true);
        if (task.empty()) return fail(res, "DOWNLOAD_NOT_FOUND", "下载任务不存在", 404);
        ok(res, task);
    });

    routes_->Get(R"(/api/v1/downloads/([^/]+)/files)", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, downloads_.files(idParam(req)));
    });

    routes_->Get(R"(/api/v1/downloads/([^/]+)/logs)", [this](const httplib::Request& req, httplib::Response& res) {
        int64_t after = 0;
        if (req.has_param("after")) { try { after = std::stoll(req.get_param_value("after")); } catch (...) {} }
        ok(res, downloads_.logs(idParam(req), after, intQuery(req, "limit", 500)));
    });

    routes_->Post(R"(/api/v1/downloads/([^/]+)/pause)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!downloads_.pause(idParam(req), error)) return fail(res, "DOWNLOAD_PAUSE_FAILED", error, 400);
        ok(res, downloads_.getTask(idParam(req), false));
    });

    routes_->Post(R"(/api/v1/downloads/([^/]+)/resume)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!downloads_.resume(idParam(req), error)) return fail(res, "DOWNLOAD_RESUME_FAILED", error, 400);
        ok(res, downloads_.getTask(idParam(req), false));
    });

    routes_->Post(R"(/api/v1/downloads/([^/]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!downloads_.cancel(idParam(req), error)) return fail(res, "DOWNLOAD_CANCEL_FAILED", error, 400);
        ok(res, downloads_.getTask(idParam(req), false));
    });

    routes_->Post(R"(/api/v1/downloads/([^/]+)/retry)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!downloads_.retry(idParam(req), error)) return fail(res, "DOWNLOAD_RETRY_FAILED", error, 400);
        ok(res, downloads_.getTask(idParam(req), false));
    });

    routes_->Delete(R"(/api/v1/downloads/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!downloads_.remove(idParam(req), error)) return fail(res, "DOWNLOAD_DELETE_FAILED", error, 400);
        ok(res, json{{"deleted", true}});
    });
}

void ApiServer::registerHuggingFaceRoutes() {
    auto statusFor = [](const std::string& code) {
        if (code == "HF_REPOSITORY_NOT_FOUND") return 404;
        if (code == "HF_TOKEN_REQUIRED" || code == "HF_FORBIDDEN") return 403;
        if (code == "HF_RATE_LIMITED") return 429;
        if (code == "HF_NETWORK_ERROR") return 502;
        return 400;
    };

    routes_->Post("/api/v1/huggingface/resolve", [this, statusFor](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        const std::string input = body.value("input", body.value("repo", ""));
        if (trim(input).empty()) return fail(res, "INVALID_REPOSITORY", "请输入 Hugging Face 仓库地址或 ID", 400);
        std::string token = body.value("token", "");
        if (token.empty()) token = downloads_.token();
        const std::string requestedType = body.value("repoType", body.value("kind", ""));
        const HfResolveResult result = HfClient::resolve(input, body.value("revision", ""), token, 30000, requestedType);
        if (!result.ok) {
            return fail(res, result.code, result.error, statusFor(result.code), result.suggestion);
        }
        json payload = result.repository.toJson();
        if (!result.details.is_null() && result.details.contains("warning")) {
            payload["warning"] = result.details["warning"];
        }
        payload["tokenUsed"] = !token.empty();
        ok(res, payload);
    });

    routes_->Get("/api/v1/huggingface/repository", [this, statusFor](const httplib::Request& req, httplib::Response& res) {
        const std::string repo = queryValue(req, "repo");
        if (repo.empty()) return fail(res, "INVALID_REPOSITORY", "缺少 repo 参数", 400);
        const HfResolveResult result = HfClient::resolve(repo, queryValue(req, "revision", "main"), downloads_.token(), 30000,
                                                         queryValue(req, "repoType", queryValue(req, "kind", "")));
        if (!result.ok) return fail(res, result.code, result.error, statusFor(result.code), result.suggestion);
        ok(res, result.repository.toJson());
    });

    routes_->Get("/api/v1/huggingface/files", [this, statusFor](const httplib::Request& req, httplib::Response& res) {
        const std::string repo = queryValue(req, "repo");
        if (repo.empty()) return fail(res, "INVALID_REPOSITORY", "缺少 repo 参数", 400);
        const HfResolveResult result = HfClient::resolve(repo, queryValue(req, "revision", "main"), downloads_.token(), 30000,
                                                         queryValue(req, "repoType", queryValue(req, "kind", "")));
        if (!result.ok) return fail(res, result.code, result.error, statusFor(result.code), result.suggestion);
        json files = json::array();
        for (const auto& file : result.repository.files) {
            files.push_back(json{{"path", file.path}, {"size", file.size}, {"category", file.category},
                                 {"sha256", file.sha256}, {"lfs", file.lfs}});
        }
        ok(res, json{{"repoId", result.repository.repoId}, {"revision", result.repository.revision},
                     {"commitHash", result.repository.commitHash}, {"files", files}});
    });

    routes_->Get("/api/v1/huggingface/token", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, downloads_.tokenStatus());
    });

    routes_->Post("/api/v1/huggingface/token/validate", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        std::string value = body.value("token", "");
        if (value.empty()) value = downloads_.token();
        const HfTokenInfo info = HfClient::validateToken(value);
        ok(res, json{{"valid", info.valid}, {"username", info.username}, {"organization", info.organization},
                     {"code", info.code}, {"message", info.error}, {"configured", downloads_.hasToken()}});
    });

    routes_->Put("/api/v1/huggingface/token", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        downloads_.setToken(body.value("token", ""), error);
        if (!error.empty()) return fail(res, "INVALID_TOKEN", error, 400);
        ok(res, downloads_.tokenStatus());
    });
}

void ApiServer::registerLlamaRoutes() {
    auto statusFor = [](const std::string& code) {
        if (code == "NO_NETWORK") return 503;
        if (code == "API_RATE_LIMITED") return 429;
        if (code == "RELEASE_NOT_FOUND" || code == "ASSET_NOT_FOUND") return 404;
        if (code == "ASSET_MISMATCH") return 409;
        return 502;
    };

    // Local state only: never touches the network, so the UI can render instantly.
    routes_->Get("/api/v1/llama/status", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, json{{"runtime", llama_.status()},
                     {"probe", llama_.probe()},
                     {"job", llama_.jobStatus()}});
    });

    routes_->Get("/api/v1/llama/probe", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, llama_.probe());
    });

    routes_->Post("/api/v1/llama/update/check", [this, statusFor](const httplib::Request& req, httplib::Response& res) {
        json body = json::object();
        std::string error;
        if (!req.body.empty() && !readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        const bool force = body.is_object() && body.contains("force") && body["force"].is_boolean()
                               ? body["force"].get<bool>()
                               : true;
        const LlamaCheckResult result = llama_.checkUpdate(force);
        if (!result.ok) return fail(res, result.code, result.error, statusFor(result.code), result.suggestion, result.data);
        ok(res, result.data);
    });

    routes_->Get("/api/v1/llama/releases/([^/]+)/assets", [this, statusFor](const httplib::Request& req, httplib::Response& res) {
        const LlamaCheckResult result = llama_.releaseAssets(idParam(req));
        if (!result.ok) return fail(res, result.code, result.error, statusFor(result.code), result.suggestion);
        ok(res, result.data);
    });

    routes_->Post("/api/v1/llama/install", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        if (!llama_.install(body, error)) return fail(res, "INSTALL_START_FAILED", error, 409,
                                                      "稍后重试，或先取消正在运行的安装任务。");
        ok(res, llama_.jobStatus(), 202);
    });

    routes_->Get("/api/v1/llama/install/status", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, json{{"job", llama_.jobStatus()}, {"runtime", llama_.status()}});
    });

    routes_->Post("/api/v1/llama/install/cancel", [this](const httplib::Request&, httplib::Response& res) {
        std::string error;
        if (!llama_.cancel(error)) return fail(res, "CANCEL_FAILED", error, 409);
        ok(res, llama_.jobStatus());
    });

    routes_->Post("/api/v1/llama/refresh", [this](const httplib::Request&, httplib::Response& res) {
        // Re-read the tool paths from disk after a manual copy of an official archive.
        std::string error;
        settings_.load();
        if (!error.empty()) return fail(res, "REFRESH_FAILED", error, 500);
        ok(res, llama_.status());
    });

    // ---- Multiple co-installed llama.cpp backends ----
    routes_->Get("/api/v1/llama/backends", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, llama_.backends());
    });

    routes_->Post("/api/v1/llama/backends/refresh", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, llama_.refreshBackends());
    });

    routes_->Post(R"(/api/v1/llama/backends/([^/]+)/activate)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!llama_.activateBackend(idParam(req), error)) return fail(res, "BACKEND_ACTIVATE_FAILED", error, 400);
        ok(res, json{{"activated", idParam(req)}, {"status", llama_.status()}, {"backends", llama_.backends()}});
    });

    routes_->Delete(R"(/api/v1/llama/backends/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!llama_.removeBackend(idParam(req), error)) {
            return fail(res, "BACKEND_REMOVE_FAILED", error, 409, "先启用另一个后端，再删除当前后端。");
        }
        ok(res, json{{"removed", true}, {"backends", llama_.backends()}});
    });
}

void ApiServer::registerQualityRoutes() {
    // Reports which corpus will be used before a run, so the UI can warn that the
    // built-in sample is not comparable with published wikitext-2 numbers.
    routes_->Get("/api/v1/quality/dataset", [this](const httplib::Request&, httplib::Response& res) {
        const AppSettings s = settings_.get();
        ok(res, json{{"dataset", quality_.dataset()},
                     {"pplPath", s.llamaPplPath},
                     {"pplAvailable", !s.llamaPplPath.empty()}});
    });

    routes_->Get("/api/v1/quality/ppl", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, quality_.results(intQuery(req, "limit", 100)));
    });

    routes_->Get("/api/v1/quality/ppl/status", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, quality_.status());
    });

    routes_->Post("/api/v1/quality/ppl/run", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string parseError;
        if (!readJson(req, body, parseError)) { fail(res, "INVALID_JSON", parseError, 400); return; }
        std::string error;
        if (!quality_.run(body, error)) {
            const bool conflict = error.find("正在进行") != std::string::npos;
            fail(res, conflict ? "PPL_BUSY" : "PPL_NOT_AVAILABLE", error, conflict ? 409 : 400);
            return;
        }
        ok(res, quality_.status(), 202);
    });

    routes_->Post("/api/v1/quality/ppl/cancel", [this](const httplib::Request&, httplib::Response& res) {
        std::string error;
        if (!quality_.cancel(error)) return fail(res, "CANCEL_FAILED", error, 409);
        ok(res, quality_.status());
    });

    routes_->Delete(R"(/api/v1/quality/ppl/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!quality_.remove(idParam(req), error)) return fail(res, "PPL_NOT_FOUND", error, 404);
        ok(res, json{{"deleted", idParam(req)}});
    });

    // Model-level view: every produced GGUF with its latest PPL and throughput.
    routes_->Get("/api/v1/quality/compare", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string modelId = queryValue(req, "modelId");
        if (modelId.empty()) return fail(res, "MODEL_ID_REQUIRED", "必须提供 modelId", 400);
        json data = quality_.comparison(modelId);
        if (data.is_null()) return fail(res, "MODEL_NOT_FOUND", "模型不存在", 404);
        ok(res, data);
    });
}

void ApiServer::registerWatchRoutes() {
    routes_->Get("/api/v1/watch/status", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, watch_.status());
    });

    routes_->Get("/api/v1/watch/events", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, watch_.events(intQuery(req, "limit", 100)));
    });

    routes_->Post("/api/v1/watch/scan", [this](const httplib::Request& req, httplib::Response& res) {
        json body = json::object();
        std::string parseError;
        if (req.body.size() > 0 && !readJson(req, body, parseError)) {
            fail(res, "INVALID_JSON", parseError, 400);
            return;
        }
        const bool force = body.value("force", false);
        std::string error;
        json summary = watch_.scan(force, error);
        if (summary.is_null()) return fail(res, "WATCH_SCAN_FAILED", error, 400);
        ok(res, summary);
    });
}

bool ApiServer::listen(std::string& error, int portOverride) {
    const AppSettings s = settings_.get();
    const int port = portOverride > 0 ? portOverride : s.port;
    Logger::instance().info("HTTP listening on " + s.bindAddress + ":" + std::to_string(port));
    if (!server_->listen(s.bindAddress, port)) {
        error = "HTTP 端口监听失败: " + s.bindAddress + ":" + std::to_string(port);
        return false;
    }
    actualPort_ = port;
    return true;
}

void ApiServer::setRuntimePort(int actualPort, const std::string& reason) {
    actualPort_ = actualPort;
    portFallbackReason_ = reason;
}

void ApiServer::stop() {
    if (server_) server_->stop();
}

bool ApiServer::running() const {
    return server_ && server_->is_running();
}

int ApiServer::port() const {
    return actualPort_ > 0 ? actualPort_ : settings_.get().port;
}

void ApiServer::registerUpdateRoutes() {
    routes_->Get("/api/v1/updates/check", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, update_.check(settings_.get().updateChannel).toJson());
    });

    // Startup check: never blocks or fails the boot. Returns only actionable
    // updates; offline/rate-limited/up-to-date outcomes are reported as failures
    // or omitted so the UI shows no banner.
    routes_->Get("/api/v1/updates/startup", [this](const httplib::Request& req, httplib::Response& res) {
        const AppSettings s = settings_.get();
        json out{{"enabled", s.autoCheckUpdates},
                 {"channel", s.updateChannel},
                 {"llamaChannel", s.llamaChannel},
                 {"checkedAt", isoNow()},
                 {"items", json::array()},
                 {"failures", json::array()}};
        if (!s.autoCheckUpdates) return ok(res, out);
        const bool force = queryValue(req, "force", "0") == "1" || queryValue(req, "force", "") == "true";

        const UpdateInfo app = update_.check(s.updateChannel);
        if (app.updateAvailable) {
            out["items"].push_back(json{{"component", "application"},
                                        {"name", "ModelForge"},
                                        {"currentVersion", app.currentVersion},
                                        {"latestVersion", app.latestVersion},
                                        {"channel", app.channel},
                                        {"prerelease", app.prerelease},
                                        {"releaseName", app.releaseName},
                                        {"publishedAt", app.publishedAt},
                                        {"htmlUrl", app.htmlUrl},
                                        {"assetName", app.assetName},
                                        {"assetSize", app.assetSize},
                                        {"message", app.message}});
        } else if (app.code != "UP_TO_DATE" && app.code != "UPDATE_AVAILABLE") {
            out["failures"].push_back(json{{"component", "application"}, {"code", app.code},
                                          {"message", app.message}});
        }

        const LlamaCheckResult llama = llama_.checkUpdate(force);
        if (!llama.ok) {
            out["failures"].push_back(json{{"component", "llama"}, {"code", llama.code}, {"message", llama.error}});
        } else {
            const std::string status = llama.data.value("status", "");
            if (status == "update_available") {
                const json& local = llama.data["local"];
                const json& asset = llama.data["asset"];
                const json& release = llama.data["release"];
                out["items"].push_back(json{{"component", "llama"},
                                            {"name", "llama.cpp"},
                                            {"currentVersion", local.value("tag", "")},
                                            {"latestVersion", llama.data.value("remoteTag", "")},
                                            {"channel", s.llamaChannel},
                                            {"prerelease", release.value("prerelease", false)},
                                            {"releaseName", release.value("name", "")},
                                            {"publishedAt", release.value("publishedAt", "")},
                                            {"htmlUrl", release.value("htmlUrl", "")},
                                            {"tag", llama.data.value("remoteTag", "")},
                                            {"assetName", asset.value("name", "")},
                                            {"assetSize", asset.value("size", 0ull)},
                                            {"message", llama.data.value("message", "")}});
            }
        }
        out["available"] = out["items"].size();
        ok(res, out);
    });

    routes_->Post("/api/v1/updates/download", [this](const httplib::Request&, httplib::Response& res) {
        std::string code;
        std::string message;
        const std::string id = update_.startDownload(code, message);
        if (id.empty()) {
            fail(res, code.empty() ? "DOWNLOAD_FAILED" : code,
                 message.empty() ? "无法开始下载" : message, 409);
            return;
        }
        ok(res, json{{"jobId", id}, {"message", message}});
    });

    routes_->Get(R"(/api/v1/updates/job/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        const UpdateJob result = update_.job(idParam(req));
        if (result.id.empty()) {
            fail(res, "JOB_NOT_FOUND", "找不到更新任务", 404);
            return;
        }
        ok(res, result.toJson());
    });

    routes_->Post("/api/v1/updates/apply", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) {
            fail(res, "INVALID_JSON", error, 400);
            return;
        }
        const std::string jobId = body.value("jobId", "");
        if (jobId.empty()) {
            fail(res, "INVALID_REQUEST", "缺少 jobId", 400);
            return;
        }
        std::string code;
        std::string message;
        std::string scriptPath;
        if (!update_.apply(jobId, code, message, scriptPath)) {
            fail(res, code.empty() ? "APPLY_FAILED" : code, message,
                 code == "UPDATE_NOT_READY" ? 409 : 500);
            return;
        }
        ok(res, json{{"message", message}, {"scriptPath", scriptPath}});
    });
}

void ApiServer::registerOfflineRoutes() {
    routes_->Get("/api/v1/library", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, library_.list(queryValue(req, "type", "all"), queryValue(req, "q", ""),
                              intQuery(req, "limit", 200)));
    });

    routes_->Get("/api/v1/library/stats", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, library_.stats());
    });

    routes_->Post("/api/v1/library/rescan", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        const bool prune = body.value("prune", true);
        ok(res, library_.rescan(prune));
    });

    routes_->Post("/api/v1/library/export", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        std::string code;
        const json result = library_.exportBundle(body, code, error);
        if (!error.empty() && result.empty()) {
            const int status = code == "NO_RESOURCES" ? 404 : (code == "DESTINATION_NOT_WRITABLE" ? 403 : 400);
            return fail(res, code.empty() ? "EXPORT_FAILED" : code, error, status);
        }
        ok(res, result);
    });

    routes_->Post("/api/v1/library/import", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        std::string code;
        const std::string manifest = body.value("manifestPath", body.value("manifest", ""));
        const json result = library_.importBundle(manifest, body, code, error);
        if (!error.empty() && result.value("importedCount", 0) == 0) {
            const int status = code == "MANIFEST_NOT_FOUND" ? 404 : 400;
            return fail(res, code.empty() ? "IMPORT_FAILED" : code, error, status);
        }
        ok(res, result);
    });

    routes_->Get("/api/v1/library/bundles", [this](const httplib::Request& req, httplib::Response& res) {
        ok(res, library_.bundles(intQuery(req, "limit", 30)));
    });

    routes_->Post("/api/v1/library/verify", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, library_.verifyAll());
    });

    routes_->Delete(R"(/api/v1/library/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string error;
        if (!library_.forget(idParam(req), error)) return fail(res, "RESOURCE_NOT_FOUND", error, 404);
        ok(res, json{{"removed", true}, {"filesKept", true}});
    });
}

void ApiServer::registerHealthRoutes() {
    auto runCheck = [this](bool deep, bool network) { return health_.run(deep, network); };

    routes_->Get("/api/v1/health/check", [this, runCheck](const httplib::Request& req, httplib::Response& res) {
        const bool deep = queryValue(req, "deep", "0") == "1" || queryValue(req, "deep", "") == "true";
        const bool network = queryValue(req, "network", "0") == "1" || queryValue(req, "network", "") == "true";
        ok(res, runCheck(deep, network));
    });

    routes_->Post("/api/v1/health/check", [this, runCheck](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        ok(res, runCheck(body.value("deep", false), body.value("network", false)));
    });

    routes_->Get("/api/v1/health/summary", [this](const httplib::Request&, httplib::Response& res) {
        ok(res, health_.summary());
    });

    routes_->Post("/api/v1/health/fix", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        std::string error;
        if (!readJson(req, body, error)) return fail(res, "INVALID_JSON", error, 400);
        const std::string action = body.value("action", body.value("id", ""));
        if (action.empty()) return fail(res, "INVALID_REQUEST", "缺少修复动作", 400);
        std::string fixError;
        const json result = health_.fix(action, fixError);
        if (!fixError.empty() && result.empty()) return fail(res, "FIX_FAILED", fixError, 400);
        ok(res, result);
    });
}

} // namespace mf
