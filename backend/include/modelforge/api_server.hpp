#pragma once

#include "modelforge/converter.hpp"
#include "modelforge/download_manager.hpp"
#include "modelforge/event_hub.hpp"
#include "modelforge/llama_runtime.hpp"
#include "modelforge/openapi.hpp"
#include "modelforge/quality_service.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/system_service.hpp"
#include "modelforge/task_manager.hpp"
#include "modelforge/update_service.hpp"
#include "modelforge/watch_service.hpp"
#include "modelforge/ws_server.hpp"

#include "httplib.h"

#include <memory>

namespace mf {

class ApiServer {
public:
    ApiServer(SettingsService& settings, ConverterService& converter, TaskManager& tasks,
              SystemService& system, DownloadManager& downloads, LlamaRuntimeService& llama,
              QualityService& quality, WatchService& watch, EventHub& events, WsServer& ws, UpdateService& update);
    ~ApiServer();

    bool prepare(std::string& error);
    bool listen(std::string& error);
    void stop();
    bool running() const;
    int port() const;

private:
    SettingsService& settings_;
    ConverterService& converter_;
    TaskManager& tasks_;
    SystemService& system_;
    DownloadManager& downloads_;
    LlamaRuntimeService& llama_;
    QualityService& quality_;
    WatchService& watch_;
    EventHub& events_;
    WsServer& ws_;
    UpdateService& update_;
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<OpenApiRegistry> routes_;
    int64_t startedAt_ = 0;

    void registerRoutes();
    void registerSystemRoutes();
    void registerModelRoutes();
    void registerConversionRoutes();
    void registerLabRoutes();
    void registerGgufRoutes();
    void registerWorkflowRoutes();
    void registerSettingsRoutes();
    void registerDownloadRoutes();
    void registerHuggingFaceRoutes();
    void registerLlamaRoutes();
    void registerQualityRoutes();
    void registerWatchRoutes();
    void registerUpdateRoutes();
    json capabilities() const;
    bool readJson(const httplib::Request& req, json& out, std::string& error) const;
};

} // namespace mf
