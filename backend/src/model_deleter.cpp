#include "modelforge/model_deleter.hpp"

#include <algorithm>
#include <system_error>

namespace mf {

json ModelDeleteResult::toJson() const {
    json out{{"code", code}, {"message", message}};
    if (!suggestion.empty()) out["suggestion"] = suggestion;
    if (!details.is_null()) out["details"] = details;
    return out;
}

namespace model_delete {

namespace {

constexpr const char* kActiveTaskStates = "('QUEUED','RUNNING','PAUSED')";

bool jsonReferences(const json& value, const std::string& id) {
    if (value.is_string()) return value.get<std::string>() == id;
    if (value.is_array()) {
        for (const auto& item : value) {
            if (jsonReferences(item, id)) return true;
        }
        return false;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (jsonReferences(it.value(), id)) return true;
        }
    }
    return false;
}

bool pathExists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

// Windows returns localized (non UTF-8) messages through std::error_code;
// including them in a JSON response would make the serializer throw. Keep the
// text stable and put the numeric code in a separate field.
std::string describeFileError(const std::string& code, const std::error_code& ec) {
    if (code == "PERMISSION_DENIED") return "权限不足，或文件正被其他程序占用。";
    if (code == "MISSING_FILE") return "文件已不存在。";
    return "无法删除文件（系统错误码 " + std::to_string(ec.value()) + "）。";
}

uint64_t fileSize(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0ull : static_cast<uint64_t>(size);
}

} // namespace

std::string classifyFileError(const std::error_code& ec) {
    if (!ec) return "";
    if (ec == std::errc::no_such_file_or_directory) return "MISSING_FILE";
    if (ec == std::errc::permission_denied) return "PERMISSION_DENIED";
    return "FILE_DELETE_FAILED";
}

ModelDeleteResult erase(Database& db, const std::string& modelId, bool deleteFiles) {
    ModelDeleteResult result;

    const json rows = db.query("SELECT id,name,path FROM models WHERE id=?", {modelId});
    if (rows.empty()) {
        result.code = "MODEL_NOT_FOUND";
        result.message = "模型不存在或已被删除。";
        return result;
    }
    const std::string modelName = rows[0].value("name", "");
    const std::string modelPath = rows[0].value("path", "");

    // 1. Refuse while background work still depends on the model.
    const json activeTasks = db.query(
        "SELECT id,status,stage FROM conversion_tasks WHERE (model_id=? OR input_path=?) AND status IN " +
            std::string(kActiveTaskStates) + " ORDER BY created_at ASC LIMIT 20",
        {modelId, modelPath});
    if (!activeTasks.empty()) {
        result.code = "MODEL_IN_USE";
        result.message = "模型仍被 " + std::to_string(activeTasks.size()) + " 个进行中的任务引用，暂时无法删除。";
        result.suggestion = "请等待任务完成或先取消相关任务后重试。";
        result.details = json{{"tasks", activeTasks}};
        return result;
    }

    json referencingWorkflows = json::array();
    for (const auto& row : db.query("SELECT id,name,definition_json FROM workflows WHERE enabled=1")) {
        json definition;
        try {
            definition = json::parse(row.value("definition_json", "{}"));
        } catch (const std::exception&) {
            continue;
        }
        if (jsonReferences(definition, modelId)) {
            referencingWorkflows.push_back(json{{"id", row.value("id", "")}, {"name", row.value("name", "")}});
        }
    }
    if (!referencingWorkflows.empty()) {
        result.code = "MODEL_IN_USE";
        result.message = "模型仍被 " + std::to_string(referencingWorkflows.size()) + " 个工作流引用，暂时无法删除。";
        result.suggestion = "请先在工作流中移除对该模型的引用后重试。";
        result.details = json{{"workflows", referencingWorkflows}};
        return result;
    }

    json details{{"modelId", modelId}, {"modelName", modelName}, {"deleteFiles", deleteFiles}};
    if (!deleteFiles) {
        std::string dbError;
        db.query("DELETE FROM models WHERE id=?", {modelId}, &dbError);
        if (!dbError.empty()) {
            result.code = "DELETE_FAILED";
            result.message = "删除模型记录失败：" + dbError;
            result.details = details;
            return result;
        }
        details["removedFiles"] = 0;
        details["freedBytes"] = 0;
        result.ok = true;
        result.message = "已删除模型“" + modelName + "”的记录，磁盘文件未改动。";
        result.details = details;
        return result;
    }

    // Artifacts produced from this model stay on disk on purpose: they may live
    // in the user's output directory and be useful on their own.
    json keptArtifacts = json::array();
    for (const auto& row : db.query("SELECT path FROM model_versions WHERE model_id=?", {modelId})) {
        const std::string path = row.value("path", "");
        if (!path.empty() && pathExists(pathFromUtf8(path))) keptArtifacts.push_back(path);
    }

    const fs::path root = fs::weakly_canonical(pathFromUtf8(modelPath));
    json removedFiles = json::array();
    json missingFiles = json::array();
    json sharedFiles = json::array();
    json unsafeFiles = json::array();
    json failures = json::array();
    uint64_t freedBytes = 0;

    // A directory registered by more than one model record is never touched.
    const json otherOwners = db.query("SELECT id FROM models WHERE id<>? AND path=?", {modelId, modelPath});
    const bool directoryShared = !otherOwners.empty();

    std::vector<std::string> candidates;
    for (const auto& row : db.query("SELECT path FROM model_files WHERE model_id=?", {modelId})) {
        const std::string path = row.value("path", "");
        if (!path.empty()) candidates.push_back(path);
    }
    // Defensive: older rows may point models.path straight at a file.
    {
        std::error_code ec;
        if (fs::is_regular_file(pathFromUtf8(modelPath), ec)) {
            candidates.push_back(modelPath);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (const std::string& candidate : candidates) {
        const fs::path path = pathFromUtf8(candidate);

        if (directoryShared || !isPathInside(root, path)) {
            // Either another model owns the same directory, or the row does not
            // belong to this model directory. Never delete in that case.
            if (directoryShared) sharedFiles.push_back(candidate);
            else unsafeFiles.push_back(candidate);
            continue;
        }

        const json owners = db.query(
            "SELECT model_id FROM model_files WHERE path=? AND model_id<>? LIMIT 1", {candidate, modelId});
        if (!owners.empty()) {
            sharedFiles.push_back(candidate);
            continue;
        }

        std::error_code existsError;
        if (!fs::exists(path, existsError) || existsError == std::errc::no_such_file_or_directory) {
            missingFiles.push_back(candidate);
            continue;
        }

        const uint64_t size = fs::is_regular_file(path) ? fileSize(path) : 0;
        std::error_code removeError;
        fs::remove(path, removeError);
        if (removeError) {
            const std::string code = classifyFileError(removeError);
            if (code == "MISSING_FILE") {
                missingFiles.push_back(candidate);
                continue;
            }
            failures.push_back(json{{"path", candidate},
                                    {"code", code},
                                    {"message", describeFileError(code, removeError)},
                                    {"systemCode", removeError.value()}});
            continue;
        }
        removedFiles.push_back(candidate);
        freedBytes += size;
    }

    details["removedFiles"] = removedFiles.size();
    details["removedPaths"] = removedFiles;
    details["freedBytes"] = freedBytes;
    details["freedLabel"] = humanBytes(freedBytes);
    details["missingFiles"] = missingFiles;
    details["sharedFiles"] = sharedFiles;
    details["unsafeFiles"] = unsafeFiles;
    details["keptArtifacts"] = keptArtifacts;
    details["failures"] = failures;

    if (!failures.empty()) {
        bool permissionOnly = true;
        for (const auto& failure : failures) {
            if (failure.value("code", "") != "PERMISSION_DENIED") permissionOnly = false;
        }
        result.code = permissionOnly ? "PERMISSION_DENIED" : "FILE_DELETE_FAILED";
        result.message = "有 " + std::to_string(failures.size()) + " 个模型文件无法删除，模型记录已保留。";
        result.suggestion = permissionOnly
                                ? "当前账户没有删除这些文件的权限，请检查文件属主或改为手动删除后重试。"
                                : "请确认文件未被其他程序占用、磁盘未只读，然后重试。";
        result.details = details;
        return result;
    }

    // Only remove the directory when nothing else lives there.
    {
        std::error_code emptyError;
        std::error_code dirError;
        if (fs::is_directory(root, dirError) && fs::is_empty(root, emptyError) && !emptyError) {
            fs::remove(root, dirError);
        }
    }

    std::string dbError;
    db.transaction([&]() {
        db.query("DELETE FROM model_files WHERE model_id=?", {modelId});
        db.query("DELETE FROM model_versions WHERE model_id=?", {modelId});
        db.query("DELETE FROM benchmark_results WHERE model_id=?", {modelId});
        db.query("DELETE FROM models WHERE id=?", {modelId}, &dbError);
    });
    if (!dbError.empty()) {
        result.code = "DELETE_FAILED";
        result.message = "模型文件已删除，但清理数据库记录失败：" + dbError;
        result.suggestion = "请重试删除操作以清理残留记录。";
        result.details = details;
        return result;
    }

    json warnings = json::array();
    if (!missingFiles.empty()) {
        warnings.push_back(std::to_string(missingFiles.size()) + " 个文件在磁盘上已不存在（记录已清理）。");
    }
    if (!sharedFiles.empty()) {
        warnings.push_back(std::to_string(sharedFiles.size()) + " 个文件仍被其他模型引用，已保留。");
    }
    if (!unsafeFiles.empty()) {
        warnings.push_back(std::to_string(unsafeFiles.size()) + " 个路径不在模型目录内，为安全起见未删除。");
    }
    if (!keptArtifacts.empty()) {
        warnings.push_back("保留了 " + std::to_string(keptArtifacts.size()) + " 个由该模型生成的 GGUF 产物。");
    }
    details["warnings"] = warnings;

    result.ok = true;
    result.message = "已删除模型“" + modelName + "”及其 " + std::to_string(removedFiles.size()) +
                     " 个文件（释放 " + humanBytes(freedBytes) + "）。";
    if (removedFiles.empty()) {
        result.message = "已删除模型“" + modelName + "”的记录（没有可删除的文件）。";
    }
    result.details = details;
    return result;
}

} // namespace model_delete
} // namespace mf
