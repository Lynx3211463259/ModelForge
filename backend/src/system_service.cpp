#include "modelforge/system_service.hpp"

#include "modelforge/model_scanner.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace mf {

SystemService::SystemService(Database& db, SettingsService& settings, ConverterService& converter, TaskManager& tasks)
    : db_(db), settings_(settings), converter_(converter), tasks_(tasks) {}

void SystemService::refreshCpu(uint64_t& idle, uint64_t& total) {
#ifdef _WIN32
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        auto toU64 = [](const FILETIME& ft) {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        idle = toU64(idleTime);
        total = toU64(kernelTime) + toU64(userTime);
    } else {
        idle = 0;
        total = 0;
    }
#else
    idle = 0;
    total = 0;
    std::ifstream input("/proc/stat");
    std::string cpu;
    uint64_t user = 0, nice = 0, system = 0, idleValue = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (input >> cpu >> user >> nice >> system >> idleValue >> iowait >> irq >> softirq >> steal) {
        idle = idleValue + iowait;
        total = user + nice + system + idleValue + iowait + irq + softirq + steal;
    }
#endif
}

json SystemService::resources() {
    std::lock_guard<std::mutex> lock(resourceMutex_);
    uint64_t idle = 0, total = 0;
    refreshCpu(idle, total);
    if (previousCpuSampleMs_ == 0) {
        previousCpuIdle_ = idle;
        previousCpuTotal_ = total;
        previousCpuSampleMs_ = nowMs();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        refreshCpu(idle, total);
    }
    double cpu = 0.0;
    if (previousCpuTotal_ > 0 && total > previousCpuTotal_) {
        const uint64_t totalDelta = total - previousCpuTotal_;
        const uint64_t idleDelta = idle - previousCpuIdle_;
        cpu = totalDelta > 0 ? (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0 : 0.0;
    }
    previousCpuIdle_ = idle;
    previousCpuTotal_ = total;
    previousCpuSampleMs_ = nowMs();
    const uint64_t totalMem = totalSystemMemory();
    const uint64_t availMem = availableSystemMemory();
    const uint64_t usedMem = totalMem > availMem ? totalMem - availMem : 0;
    const fs::path output = settings_.outputDir();
    return json{
        {"cpuPercent", std::round(cpu * 10.0) / 10.0},
        {"memoryTotal", totalMem}, {"memoryAvailable", availMem}, {"memoryUsed", usedMem},
        {"memoryPercent", totalMem ? std::round((static_cast<double>(usedMem) / totalMem) * 1000.0) / 10.0 : 0.0},
        {"diskFree", diskFreeBytes(output)}, {"diskTotal", diskTotalBytes(output)},
        {"gpu", detectGpuSummary()}, {"os", getOsName()}, {"host", getHostName()},
        {"threads", settings_.get().threads}, {"sampledAt", isoNow()}
    };
}

json SystemService::modelFromRow(const json& row, bool includeFiles) {
    json model{
        {"id", row.value("id", "")}, {"name", row.value("name", "")}, {"path", row.value("path", "")},
        {"architecture", row.value("architecture", "")}, {"architectureFamily", row.value("architecture_family", "")},
        {"parameterCount", row.value("parameter_count", 0ll)}, {"dtype", row.value("dtype", "")},
        {"totalSize", row.value("total_size", 0ll)}, {"shardCount", row.value("shard_count", 0)},
        {"tensorCount", row.value("tensor_count", 0)}, {"status", row.value("status", "")},
        {"supported", row.value("supported", 0) != 0}, {"favorite", row.value("favorite", 0) != 0},
        {"notes", row.value("notes", "")}, {"tags", row.value("tags", "")},
        {"createdAt", row.value("created_at", "")}, {"updatedAt", row.value("updated_at", "")}
    };
    try { model["analysis"] = json::parse(row.value("analysis_json", "{}")); } catch (...) { model["analysis"] = json::object(); }
    if (includeFiles) {
        json files = json::array();
        for (const auto& file : db_.query("SELECT * FROM model_files WHERE model_id=? ORDER BY kind,file_name", {model["id"]})) {
            files.push_back(json{{"name", file.value("file_name", "")}, {"path", file.value("path", "")},
                                 {"size", file.value("size", 0ll)}, {"kind", file.value("kind", "")},
                                 {"valid", file.value("valid", 1) != 0}, {"sha256", file.value("sha256", "")}});
        }
        model["files"] = files;
        json versions = json::array();
        for (const auto& version : db_.query("SELECT * FROM model_versions WHERE model_id=? ORDER BY created_at DESC LIMIT 50", {model["id"]})) {
            std::error_code versionEc;
            const std::string versionPath = version.value("path", "");
            versions.push_back(json{{"id", version.value("id", "")}, {"taskId", version.value("task_id", "")},
                                    {"quantType", version.value("quant_type", "")}, {"path", versionPath},
                                    {"size", version.value("size", 0ll)}, {"sha256", version.value("sha256", "")},
                                    {"parentId", version.value("parent_id", "")},
                                    {"toolVersion", version.value("tool_version", "")},
                                    {"exists", !versionPath.empty() && fs::is_regular_file(pathFromUtf8(versionPath), versionEc)},
                                    {"createdAt", version.value("created_at", "")}});
        }
        model["versions"] = versions;
    }
    return model;
}

json SystemService::modelVersions(const std::string& id) {
    const json model = getModel(id);
    if (model.is_null()) return nullptr;

    json children = json::array();
    json nodes = json::array();
    uint64_t totalSize = 0;
    int missing = 0;

    for (const auto& row : db_.query(
             "SELECT * FROM model_versions WHERE model_id=? ORDER BY created_at ASC LIMIT 200", {id})) {
        const std::string path = row.value("path", "");
        std::error_code ec;
        const bool exists = !path.empty() && fs::is_regular_file(pathFromUtf8(path), ec);
        if (!exists) ++missing;
        const uint64_t size = static_cast<uint64_t>(row.value("size", 0ll));
        totalSize += size;
        json node{{"id", row.value("id", "")},
                  {"label", row.value("quant_type", "").empty() ? std::string("GGUF") : row.value("quant_type", "")},
                  {"kind", "artifact"},
                  {"taskId", row.value("task_id", "")},
                  {"quantType", row.value("quant_type", "")},
                  {"path", path},
                  {"size", size},
                  {"sha256", row.value("sha256", "")},
                  {"toolVersion", row.value("tool_version", "")},
                  {"parentId", row.value("parent_id", "")},
                  {"exists", exists},
                  {"createdAt", row.value("created_at", "")},
                  {"children", json::array()}};
        children.push_back(node);
        nodes.push_back(node);
    }

    json root{{"id", "model:" + id},
              {"label", model.value("name", "")},
              {"kind", "source"},
              {"taskId", ""},
              {"quantType", model.value("dtype", "")},
              {"path", model.value("path", "")},
              {"size", model.value("totalSize", 0ll)},
              {"sha256", ""},
              {"toolVersion", ""},
              {"parentId", ""},
              {"exists", true},
              {"createdAt", model.value("createdAt", "")},
              {"children", children}};
    nodes.insert(nodes.begin(), root);

    return json{{"modelId", id},
                {"modelName", model.value("name", "")},
                {"root", root},
                {"nodes", nodes},
                {"totals", json{{"count", children.size()},
                                {"size", totalSize},
                                {"missing", missing}}}};
}

json SystemService::summary() {
    const uint64_t modelCount = db_.query("SELECT COUNT(*) AS n FROM models").empty() ? 0 :
                                db_.query("SELECT COUNT(*) AS n FROM models")[0].value("n", 0ll);
    const uint64_t totalSize = db_.query("SELECT COALESCE(SUM(total_size),0) AS n FROM models").empty() ? 0 :
                               db_.query("SELECT COALESCE(SUM(total_size),0) AS n FROM models")[0].value("n", 0ll);
    const uint64_t successCount = db_.query("SELECT COUNT(*) AS n FROM conversion_tasks WHERE status='SUCCEEDED'").empty() ? 0 :
                                  db_.query("SELECT COUNT(*) AS n FROM conversion_tasks WHERE status='SUCCEEDED'")[0].value("n", 0ll);
    const uint64_t outputSize = db_.query("SELECT COALESCE(SUM(output_size),0) AS n FROM conversion_tasks WHERE status='SUCCEEDED'").empty() ? 0 :
                                db_.query("SELECT COALESCE(SUM(output_size),0) AS n FROM conversion_tasks WHERE status='SUCCEEDED'")[0].value("n", 0ll);
    json recentModels = json::array();
    for (const auto& row : db_.query("SELECT * FROM models ORDER BY created_at DESC LIMIT 6")) recentModels.push_back(modelFromRow(row, false));
    return json{
        {"models", modelCount}, {"totalModelSize", totalSize}, {"conversionsSucceeded", successCount},
        {"convertedOutputSize", outputSize}, {"queue", tasks_.queueStats()}, {"resources", resources()},
        {"recentTasks", tasks_.listTasks("", "", 8)}, {"recentModels", recentModels},
        {"engine", converter_.capabilities().toJson()}
    };
}

json SystemService::listModels(const std::string& query, bool favoritesOnly, int limit) {
    std::string sql = "SELECT * FROM models WHERE 1=1";
    std::vector<json> params;
    if (!query.empty()) {
        sql += " AND (LOWER(name) LIKE ? OR LOWER(path) LIKE ? OR LOWER(architecture) LIKE ?)";
        const std::string like = "%" + toLower(query) + "%";
        params.push_back(like); params.push_back(like); params.push_back(like);
    }
    if (favoritesOnly) sql += " AND favorite=1";
    sql += " ORDER BY favorite DESC,datetime(updated_at) DESC,created_at DESC LIMIT ?";
    params.push_back(std::max(1, std::min(limit, 500)));
    json result = json::array();
    for (const auto& row : db_.query(sql, params)) result.push_back(modelFromRow(row, false));
    return result;
}

json SystemService::getModel(const std::string& id) {
    const auto rows = db_.query("SELECT * FROM models WHERE id=?", {id});
    if (rows.empty()) return nullptr;
    json model = modelFromRow(rows[0], true);
    model["recentTasks"] = tasks_.listTasks("", id, 20);
    return model;
}

json SystemService::importModel(const std::string& pathValue, std::string& code, std::string& error) {
    if (pathValue.empty()) { code = "PATH_REQUIRED"; error = "必须提供模型目录路径"; return nullptr; }
    ModelScanResult scan;
    try {
        scan = ModelScanner::scan(pathFromUtf8(pathValue));
    } catch (const std::exception& ex) {
        code = "SCAN_FAILED";
        error = ex.what();
        return nullptr;
    }
    if (!scan.errors.empty() && scan.shardCount == 0) {
        code = "MODEL_NOT_FOUND";
        error = scan.errors.front();
        return nullptr;
    }
    const json compatibility = converter_.compatibility(scan);
    const bool supported = compatibility.value("supported", false);
    const auto existing = db_.query("SELECT id FROM models WHERE path=?", {scan.path});
    std::string id;
    if (!existing.empty()) {
        id = existing[0].value("id", "");
        db_.query("UPDATE models SET name=?,architecture=?,architecture_family=?,parameter_count=?,dtype=?,total_size=?,"
                  "shard_count=?,tensor_count=?,status=?,supported=?,analysis_json=?,config_json=?,updated_at=? WHERE id=?",
                  {scan.name, scan.architecture, scan.architectureFamily, static_cast<int64_t>(scan.parameterCount),
                   scan.dtype, static_cast<int64_t>(scan.totalSize), scan.shardCount, scan.tensorCount,
                   scan.complete ? "READY" : "INCOMPLETE", supported ? 1 : 0,
                   ModelScanner::buildInspectionReport(scan).dump(), scan.config.dump(), isoNow(), id});
        db_.query("DELETE FROM model_files WHERE model_id=?", {id});
    } else {
        id = newId("model");
        db_.query("INSERT INTO models(id,name,path,architecture,architecture_family,parameter_count,dtype,total_size,shard_count,"
                  "tensor_count,status,supported,favorite,notes,tags,analysis_json,config_json,created_at,updated_at) "
                  "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,'','',?,?,?,?)",
                  {id, scan.name, scan.path, scan.architecture, scan.architectureFamily,
                   static_cast<int64_t>(scan.parameterCount), scan.dtype, static_cast<int64_t>(scan.totalSize),
                   scan.shardCount, scan.tensorCount, scan.complete ? "READY" : "INCOMPLETE", supported ? 1 : 0,
                   ModelScanner::buildInspectionReport(scan).dump(), scan.config.dump(), isoNow(), isoNow()}, &error);
    }
    if (!error.empty()) { code = "DB_ERROR"; return nullptr; }
    for (const auto& file : scan.files) {
        db_.query("INSERT INTO model_files(model_id,path,file_name,size,kind,valid,details_json) VALUES(?,?,?,?,?,?,?)",
                  {id, file.value("path", ""), file.value("name", ""), file.value("size", 0ll),
                   file.value("kind", ""), file.value("valid", true) ? 1 : 0, file.dump()});
    }
    db_.query("INSERT INTO conversion_history(id,model_id,task_id,event,detail,created_at) VALUES(?,?,?,?,?,?)",
              {newId("hist"), id, "", "MODEL_IMPORTED", "导入模型: " + scan.name, isoNow()});
    json model = getModel(id);
    if (model.is_object()) {
        model["compatibility"] = compatibility;
        model["warnings"] = scan.warnings;
        model["errors"] = scan.errors;
    }
    return model;
}

json SystemService::reanalyzeModel(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT path FROM models WHERE id=?", {id});
    if (rows.empty()) { error = "模型不存在"; return nullptr; }
    std::string code;
    json result = importModel(rows[0].value("path", ""), code, error);
    if (result.is_null() && error.empty()) error = code;
    return result;
}

bool SystemService::updateModel(const std::string& id, const json& patch, std::string& error) {
    const auto rows = db_.query("SELECT id FROM models WHERE id=?", {id});
    if (rows.empty()) { error = "模型不存在"; return false; }
    if (patch.contains("favorite")) {
        db_.query("UPDATE models SET favorite=?,updated_at=? WHERE id=?", {patch["favorite"].get<bool>() ? 1 : 0, isoNow(), id});
    }
    if (patch.contains("notes") && patch["notes"].is_string()) {
        db_.query("UPDATE models SET notes=?,updated_at=? WHERE id=?", {patch["notes"].get<std::string>(), isoNow(), id});
    }
    if (patch.contains("tags") && patch["tags"].is_string()) {
        db_.query("UPDATE models SET tags=?,updated_at=? WHERE id=?", {patch["tags"].get<std::string>(), isoNow(), id});
    }
    if (patch.contains("name") && patch["name"].is_string() && !patch["name"].get<std::string>().empty()) {
        db_.query("UPDATE models SET name=?,updated_at=? WHERE id=?", {patch["name"].get<std::string>(), isoNow(), id});
    }
    return true;
}

ModelDeleteResult SystemService::deleteModel(const std::string& id, bool deleteFiles) {
    return model_delete::erase(db_, id, deleteFiles);
}

json SystemService::history(int limit) {
    json items = json::array();
    limit = std::max(1, std::min(limit, 500));
    for (const auto& row : db_.query("SELECT * FROM conversion_history ORDER BY created_at DESC LIMIT ?", {limit})) {
        items.push_back(json{{"id", row.value("id", "")}, {"modelId", row.value("model_id", "")},
                             {"taskId", row.value("task_id", "")}, {"event", row.value("event", "")},
                             {"detail", row.value("detail", "")}, {"createdAt", row.value("created_at", "")}});
    }
    return items;
}

json SystemService::quantizationProfiles() {
    json result = json::array();
    for (const auto& row : db_.query("SELECT * FROM quantization_profiles ORDER BY builtin DESC,created_at ASC")) {
        json item{{"id", row.value("id", "")}, {"name", row.value("name", "")},
                  {"description", row.value("description", "")}, {"builtin", row.value("builtin", 0) != 0},
                  {"createdAt", row.value("created_at", "")}};
        try { item["params"] = json::parse(row.value("params_json", "{}")); } catch (...) { item["params"] = json::object(); }
        result.push_back(item);
    }
    return result;
}

json SystemService::saveQuantizationProfile(const std::string& id, const json& body) {
    const std::string name = sanitizeSingleLine(body.value("name", std::string("")), 120);
    if (name.empty()) return nullptr;
    json params = body.contains("params") && body["params"].is_object() ? body["params"] : json::object();
    if (params.empty()) {
        for (const char* key : {"outType", "quantType", "threads", "verify", "computeSha256"}) {
            if (body.contains(key)) params[key] = body[key];
        }
    }
    const std::string description = sanitizeSingleLine(body.value("description", std::string("")), 1000);
    if (id.empty()) {
        const std::string newIdValue = newId("profile");
        db_.query("INSERT INTO quantization_profiles(id,name,description,params_json,builtin,created_at) VALUES(?,?,?,?,0,?)",
                  {newIdValue, name, description, params.dump(), isoNow()});
        for (const auto& item : quantizationProfiles()) if (item.value("id", "") == newIdValue) return item;
        return nullptr;
    }
    const auto rows = db_.query("SELECT builtin FROM quantization_profiles WHERE id=?", {id});
    if (rows.empty() || rows[0].value("builtin", 0) != 0) return nullptr;
    db_.query("UPDATE quantization_profiles SET name=?,description=?,params_json=? WHERE id=?",
              {name, description, params.dump(), id});
    for (const auto& item : quantizationProfiles()) if (item.value("id", "") == id) return item;
    return nullptr;
}

bool SystemService::deleteQuantizationProfile(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT builtin FROM quantization_profiles WHERE id=?", {id});
    if (rows.empty()) { error = "预设不存在"; return false; }
    if (rows[0].value("builtin", 0) != 0) { error = "内置预设不可删除"; return false; }
    db_.query("DELETE FROM quantization_profiles WHERE id=?", {id});
    return true;
}

json SystemService::benchmarks(int limit) {
    json result = json::array();
    limit = std::max(1, std::min(limit, 200));
    for (const auto& row : db_.query("SELECT * FROM benchmark_results ORDER BY created_at DESC LIMIT ?", {limit})) {
        json item{{"id", row.value("id", "")}, {"modelId", row.value("model_id", "")},
                  {"quantType", row.value("quant_type", "")}, {"createdAt", row.value("created_at", "")}};
        try { item["metrics"] = json::parse(row.value("metrics_json", "[]")); } catch (...) { item["metrics"] = json::array(); }
        try { item["environment"] = json::parse(row.value("environment_json", "{}")); } catch (...) { item["environment"] = json::object(); }
        result.push_back(item);
    }
    return result;
}

json SystemService::saveBenchmark(const std::string& path, const std::string& modelId, const json& metrics, const std::string& raw, const std::string& requestedQuant) {
    const std::string id = newId("bench");
    std::string quantType = requestedQuant;
    if (quantType.empty() && metrics.is_array() && !metrics.empty() && metrics[0].is_object()) {
        quantType = metrics[0].value("type", "");
        if (quantType.empty()) {
            const std::string modelType = metrics[0].value("model_type", "");
            const std::size_t marker = modelType.find(" Q");
            if (marker != std::string::npos) quantType = modelType.substr(marker + 1);
        }
    }
    json environment{{"os", getOsName()}, {"host", getHostName()}, {"rawOutput", raw.substr(0, 200000)},
                     {"method", "llama-bench -p 512 -n 128 -r 1 -o json"}};
    db_.query("INSERT INTO benchmark_results(id,model_id,version_id,quant_type,metrics_json,environment_json,created_at) "
              "VALUES(?,?,?,?,?,?,?)",
              {id, modelId, "", quantType, metrics.dump(), environment.dump(), isoNow()});
    return json{{"id", id}, {"path", path}, {"quantType", quantType}, {"metrics", metrics}, {"environment", environment}};
}

json SystemService::listWorkflows() {
    json result = json::array();
    for (const auto& row : db_.query("SELECT * FROM workflows ORDER BY updated_at DESC")) {
        json item{{"id", row.value("id", "")}, {"name", row.value("name", "")},
                  {"description", row.value("description", "")}, {"enabled", row.value("enabled", 1) != 0},
                  {"createdAt", row.value("created_at", "")}, {"updatedAt", row.value("updated_at", "")}};
        try { item["definition"] = json::parse(row.value("definition_json", "{}")); } catch (...) { item["definition"] = json::object(); }
        result.push_back(item);
    }
    return result;
}

json SystemService::getWorkflow(const std::string& id) {
    const auto rows = db_.query("SELECT * FROM workflows WHERE id=?", {id});
    if (rows.empty()) return nullptr;
    json item{{"id", rows[0].value("id", "")}, {"name", rows[0].value("name", "")},
              {"description", rows[0].value("description", "")}, {"enabled", rows[0].value("enabled", 1) != 0},
              {"createdAt", rows[0].value("created_at", "")}, {"updatedAt", rows[0].value("updated_at", "")}};
    try { item["definition"] = json::parse(rows[0].value("definition_json", "{}")); } catch (...) { item["definition"] = json::object(); }
    return item;
}

json SystemService::saveWorkflow(const std::string& id, const json& body) {
    const std::string name = sanitizeSingleLine(body.value("name", std::string("")), 120);
    if (name.empty()) return nullptr;
    json definition = body.contains("definition") && body["definition"].is_object() ? body["definition"] : json::object();
    if (definition.empty()) {
        for (const char* key : {"quantTypes", "outType", "outputDir", "threads", "overwrite", "computeSha256"}) {
            if (body.contains(key)) definition[key] = body[key];
        }
    }
    const std::string description = sanitizeSingleLine(body.value("description", std::string("")), 1000);
    std::string workflowId = id;
    if (workflowId.empty()) {
        workflowId = newId("workflow");
        db_.query("INSERT INTO workflows(id,name,description,definition_json,enabled,created_at,updated_at) VALUES(?,?,?,?,1,?,?)",
                  {workflowId, name, description, definition.dump(), isoNow(), isoNow()});
    } else {
        const auto rows = db_.query("SELECT id FROM workflows WHERE id=?", {workflowId});
        if (rows.empty()) return nullptr;
        db_.query("UPDATE workflows SET name=?,description=?,definition_json=?,updated_at=? WHERE id=?",
                  {name, description, definition.dump(), isoNow(), workflowId});
    }
    return getWorkflow(workflowId);
}

bool SystemService::deleteWorkflow(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT id FROM workflows WHERE id=?", {id});
    if (rows.empty()) { error = "工作流不存在"; return false; }
    db_.query("DELETE FROM workflows WHERE id=?", {id});
    return true;
}

} // namespace mf
