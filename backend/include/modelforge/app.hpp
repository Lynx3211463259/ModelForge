#pragma once

#include "modelforge/api_server.hpp"
#include "modelforge/update_service.hpp"
#include "modelforge/converter.hpp"
#include "modelforge/database.hpp"
#include "modelforge/download_manager.hpp"
#include "modelforge/event_hub.hpp"
#include "modelforge/health_check.hpp"
#include "modelforge/llama_runtime.hpp"
#include "modelforge/offline_library.hpp"
#include "modelforge/quality_service.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/system_service.hpp"
#include "modelforge/task_manager.hpp"
#include "modelforge/watch_service.hpp"
#include "modelforge/ws_server.hpp"

#include <memory>

namespace mf {

struct AppOptions {
    fs::path root;
    fs::path dataDir;
    bool noBrowser = false;
    int portOverride = 0;
};

class App {
public:
    explicit App(AppOptions options);
    ~App();
    bool initialize(std::string& error);
    int runServer();
    int runScan(const std::string& input, bool jsonOutput);
    int runValidate(const std::string& file, bool jsonOutput);
    int runConvert(const std::string& input, const std::string& outputDir, const std::string& outputName,
                   const std::string& outType, const std::string& quant, int threads, bool overwrite);
    int runDownloadHf(const std::string& repo, const std::string& revision, const std::string& output,
                      int concurrency, uint64_t maxSpeed, const std::string& files, const std::string& includePattern,
                      const std::string& excludePattern, bool weightsOnly, const std::string& token);
    int runDownloadList(bool jsonOutput);
    int runDownloadAction(const std::string& action, const std::string& id);
    // action: status | check | assets | install
    int runLlama(const std::string& action, const std::string& tag, const std::string& asset, bool jsonOutput);
    void stop();

private:
    AppOptions options_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<SettingsService> settings_;
    std::unique_ptr<ConverterService> converter_;
    std::unique_ptr<EventHub> events_;
    std::unique_ptr<TaskManager> tasks_;
    std::unique_ptr<SystemService> system_;
    std::unique_ptr<DownloadManager> downloads_;
    std::unique_ptr<LlamaRuntimeService> llama_;
    std::unique_ptr<QualityService> quality_;
    std::unique_ptr<WatchService> watch_;
    std::unique_ptr<OfflineLibrary> library_;
    std::unique_ptr<HealthCheckService> health_;
    std::unique_ptr<WsServer> ws_;
    std::unique_ptr<ApiServer> api_;
    std::unique_ptr<UpdateService> update_;
    bool initialized_ = false;
    bool stopped_ = false;

    fs::path detectRoot() const;
    void openBrowser(int port);
    // Picks a bindable HTTP port: preferred when free, otherwise the next free port
    // (up to +50). outcome: free | moved | unknown | exhausted.
    int resolveListenPort(int preferred, std::string& outcome) const;
    int resolveListenPort(int preferred, const std::string& host, std::string& outcome) const;
};

} // namespace mf
