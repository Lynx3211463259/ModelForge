#include "modelforge/offline_library.hpp"

#include "modelforge/crypto.hpp"
#include "modelforge/logger.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace mf {

namespace {

constexpr const char* kManifestName = "modelforge-offline-bundle.json";
constexpr const char* kManifestFormat = "modelforge-offline-bundle-v1";
constexpr const char* kSumsName = "SHA256SUMS.txt";

std::string shortId(const std::string& type, const std::string& path) {
    Sha256 hash;
    const std::string seed = type + "|" + path;
    hash.update(seed.data(), seed.size());
    return hash.hexDigest().substr(0, 16);
}

bool copyTree(const fs::path& source, const fs::path& target, uint64_t& copiedBytes, std::string& error) {
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        error = "源文件不存在: " + pathToUtf8(source);
        return false;
    }
    if (fs::is_regular_file(source, ec)) {
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "复制失败 " + pathToUtf8(source) + ": " + ec.message();
            return false;
        }
        copiedBytes += fs::file_size(target, ec);
        return true;
    }
    fs::create_directories(target, ec);
    if (ec) {
        error = "无法创建目录 " + pathToUtf8(target) + ": " + ec.message();
        return false;
    }
    for (fs::recursive_directory_iterator it(source, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            error = ec.message();
            return false;
        }
        const fs::path relative = it->path().lexically_relative(source);
        if (relative.empty()) continue;
        const fs::path dest = target / relative;
        if (it->is_directory(ec)) {
            fs::create_directories(dest, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "复制失败 " + pathToUtf8(it->path()) + ": " + ec.message();
                return false;
            }
            copiedBytes += fs::file_size(dest, ec);
        }
        if (ec) {
            error = ec.message();
            return false;
        }
    }
    return true;
}

} // namespace

OfflineLibrary::OfflineLibrary(Database& db, SettingsService& settings, LlamaRuntimeService& llama)
    : db_(db), settings_(settings), llama_(llama) {}

std::string OfflineLibrary::stableId(const std::string& type, const std::string& path) {
    return "lib-" + shortId(type, path);
}

void OfflineLibrary::directoryUsage(const fs::path& dir, uint64_t& bytes, int& files) {
    std::error_code ec;
    bytes = 0;
    files = 0;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) {
            bytes += static_cast<uint64_t>(fs::file_size(it->path(), ec));
            if (!ec) ++files;
        }
    }
}

std::string OfflineLibrary::upsertResource(const std::string& type, const std::string& name, const std::string& version,
                                          const std::string& path, uint64_t size, int fileCount,
                                          const std::string& sha256, const std::string& source,
                                          const std::string& repoId, const std::string& revision, const json& meta) {
    const std::string id = stableId(type, path);
    db_.query("INSERT INTO offline_resources(id,type,name,version,path,size,file_count,sha256,source,repo_id,revision,"
              "meta_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
              "ON CONFLICT(id) DO UPDATE SET name=excluded.name,version=excluded.version,size=excluded.size,"
              "file_count=excluded.file_count,sha256=excluded.sha256,source=excluded.source,repo_id=excluded.repo_id,"
              "revision=excluded.revision,meta_json=excluded.meta_json,updated_at=excluded.updated_at",
              {id, type, name, version, path, size, fileCount, sha256, source, repoId, revision,
               meta.is_null() ? std::string("{}") : meta.dump(), isoNow(), isoNow()});
    return id;
}

json OfflineLibrary::resourceRow(const json& row) const {
    std::error_code ec;
    const std::string path = row.value("path", "");
    const fs::path fsPath = pathFromUtf8(path);
    const bool present = !path.empty() && fs::exists(fsPath, ec);
    json meta = json::object();
    try {
        const std::string raw = row.value("meta_json", "{}");
        if (!raw.empty()) meta = json::parse(raw);
    } catch (...) {
        meta = json::object();
    }
    json out{{"id", row.value("id", "")},
             {"type", row.value("type", "")},
             {"name", row.value("name", "")},
             {"version", row.value("version", "")},
             {"path", path},
             {"size", row.value("size", 0ull)},
             {"fileCount", row.value("file_count", 0)},
             {"sha256", row.value("sha256", "")},
             {"source", row.value("source", "")},
             {"repoId", row.value("repo_id", "")},
             {"revision", row.value("revision", "")},
             {"present", present},
             {"meta", meta},
             {"createdAt", row.value("created_at", "")},
             {"updatedAt", row.value("updated_at", "")}};
    if (present) {
        uint64_t actual = 0;
        int files = 0;
        directoryUsage(fsPath, actual, files);
        if (!fs::is_directory(fsPath, ec)) {
            actual = static_cast<uint64_t>(fs::file_size(fsPath, ec));
            files = 1;
        }
        out["onDiskBytes"] = actual;
        out["onDiskFiles"] = files;
        // Recorded size is the sum of the files ModelForge tracked; extra files in the
        // folder (readme, .gitattributes, sidecars) are legitimate. Only a smaller
        // footprint than recorded means data actually disappeared.
        out["sizeMatches"] = row.value("size", 0ull) == 0 || actual >= row.value("size", 0ull);
    }
    return out;
}

json OfflineLibrary::list(const std::string& type, const std::string& query, int limit) {
    limit = std::max(1, std::min(limit, 500));
    std::string sql = "SELECT * FROM offline_resources";
    std::vector<json> params;
    std::vector<std::string> clauses;
    if (!trim(type).empty() && type != "all") {
        clauses.push_back("type=?");
        params.push_back(trim(type));
    }
    if (!trim(query).empty()) {
        clauses.push_back("(LOWER(name) LIKE ? OR LOWER(path) LIKE ? OR LOWER(repo_id) LIKE ?)");
        const std::string needle = "%" + toLower(trim(query)) + "%";
        params.push_back(needle);
        params.push_back(needle);
        params.push_back(needle);
    }
    for (size_t i = 0; i < clauses.size(); ++i) sql += (i == 0 ? " WHERE " : " AND ") + clauses[i];
    sql += " ORDER BY updated_at DESC LIMIT ?";
    params.push_back(limit);
    json items = json::array();
    for (const auto& row : db_.query(sql, params)) items.push_back(resourceRow(row));
    return json{{"items", items}, {"stats", stats()}};
}

json OfflineLibrary::stats() {
    const auto rows = db_.query("SELECT type, COUNT(*) AS n, COALESCE(SUM(size),0) AS bytes FROM offline_resources GROUP BY type");
    json byType = json::object();
    int total = 0;
    uint64_t bytes = 0;
    for (const auto& row : rows) {
        const int count = row.value("n", 0);
        const uint64_t size = row.value("bytes", 0ull);
        byType[row.value("type", "")] = json{{"count", count}, {"bytes", size}};
        total += count;
        bytes += size;
    }
    int missing = 0;
    for (const auto& row : db_.query("SELECT path FROM offline_resources")) {
        std::error_code ec;
        if (!fs::exists(pathFromUtf8(row.value("path", "")), ec)) ++missing;
    }
    return json{{"total", total}, {"bytes", bytes}, {"byType", byType}, {"missing", missing},
                {"datasetsDir", pathToUtf8(settings_.datasetsDir())},
                {"modelsDir", pathToUtf8(settings_.modelsDir())}};
}

json OfflineLibrary::rescan(bool prune) {
    std::lock_guard<std::mutex> lock(mutex_);
    int added = 0;
    std::error_code ec;
    fs::create_directories(settings_.datasetsDir(), ec);

    // 1. Local models registered in the model table.
    std::set<std::string> modelPaths;
    for (const auto& row : db_.query("SELECT id,name,path,total_size,shard_count,architecture FROM models")) {
        const std::string path = row.value("path", "");
        if (path.empty()) continue;
        if (!fs::exists(pathFromUtf8(path), ec)) continue;
        modelPaths.insert(toLower(path));
        uint64_t bytes = row.value("total_size", 0ull);
        int files = row.value("shard_count", 0);
        if (bytes == 0) directoryUsage(pathFromUtf8(path), bytes, files);
        json meta{{"modelId", row.value("id", "")}, {"architecture", row.value("architecture", "")}};
        upsertResource("model", row.value("name", ""), "", path, bytes, files, "", "local", "", "", meta);
        ++added;
    }
    if (prune) {
        for (const auto& row : db_.query("SELECT id,path FROM offline_resources WHERE type='model' AND source='local'")) {
            if (!modelPaths.count(toLower(row.value("path", "")))) {
                db_.query("DELETE FROM offline_resources WHERE id=?", {row.value("id", "")});
            }
        }
    }

    // 2. Datasets: every direct child of the datasets root is one dataset.
    if (fs::is_directory(settings_.datasetsDir(), ec)) {
        for (const auto& entry : fs::directory_iterator(settings_.datasetsDir(), ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            uint64_t bytes = 0;
            int files = 0;
            directoryUsage(entry.path(), bytes, files);
            if (files == 0) continue;
            const std::string name = pathToUtf8(entry.path().filename());
            const std::string path = pathToUtf8(fs::absolute(entry.path(), ec));
            const auto existing = db_.query("SELECT id,repo_id,revision,meta_json FROM offline_resources WHERE type='dataset' AND path=?",
                                            {path});
            std::string repoId = name;
            std::string revision = "";
            json meta = json{{"fileCount", files}};
            if (!existing.empty()) {
                repoId = existing[0].value("repo_id", name);
                revision = existing[0].value("revision", "");
                try { meta = json::parse(existing[0].value("meta_json", "{}")); } catch (...) {}
            }
            meta["scannedFileCount"] = files;
            upsertResource("dataset", repoId, revision, path, bytes, files, "", "local", repoId, revision, meta);
            ++added;
        }
    }

    // 3. Installed llama.cpp backends (each one is usable offline).
    const json backends = llama_.backends();
    for (const auto& item : backends["items"]) {
        const std::string dir = item.value("releaseDir", "");
        if (dir.empty()) continue;
        uint64_t bytes = 0;
        int files = 0;
        directoryUsage(pathFromUtf8(dir), bytes, files);
        json meta{{"backend", item.value("backend", "")},
                  {"backendLabel", item.value("backendLabel", "")},
                  {"active", item.value("active", false)},
                  {"cliPath", item.value("cliPath", "")}};
        upsertResource("backend", item.value("id", ""), item.value("tag", ""), dir, bytes, files,
                       item.value("sha256", ""), "llama.cpp", "", "", meta);
        ++added;
    }

    Logger::instance().info("[offline-library] 扫描完成，索引 " + std::to_string(added) + " 项资源");
    json result = stats();
    result["indexed"] = added;
    return result;
}

json OfflineLibrary::exportBundle(const json& options, std::string& code, std::string& error) {
    const std::string destination = trim(options.value("destination", ""));
    if (destination.empty()) {
        code = "INVALID_DESTINATION";
        error = "请选择清单/资源包的导出目录";
        return json::object();
    }
    const fs::path root = fs::absolute(pathFromUtf8(destination));
    if (!ensureDirectory(root, error)) {
        code = "INVALID_DESTINATION";
        return json::object();
    }
    if (!directoryWritable(root)) {
        code = "DESTINATION_NOT_WRITABLE";
        error = "目录不可写: " + pathToUtf8(root);
        return json::object();
    }
    const bool copyFiles = options.value("copy", false);
    const bool includeSha = options.value("includeSha256", false);
    std::set<std::string> ids;
    if (options.contains("ids") && options["ids"].is_array()) {
        for (const auto& item : options["ids"]) if (item.is_string()) ids.insert(item.get<std::string>());
    }
    std::set<std::string> types;
    if (options.contains("types") && options["types"].is_array()) {
        for (const auto& item : options["types"]) if (item.is_string()) types.insert(item.get<std::string>());
    }

    json manifestItems = json::array();
    json missing = json::array();
    std::string sums;
    uint64_t totalSize = 0;
    uint64_t copiedBytes = 0;
    int copied = 0;
    for (const auto& row : db_.query("SELECT * FROM offline_resources ORDER BY type, name")) {
        const std::string id = row.value("id", "");
        const std::string type = row.value("type", "");
        if (!ids.empty() && !ids.count(id)) continue;
        if (!types.empty() && !types.count(type)) continue;
        const std::string path = row.value("path", "");
        const fs::path source = pathFromUtf8(path);
        std::error_code ec;
        if (path.empty() || !fs::exists(source, ec)) {
            missing.push_back(json{{"id", id}, {"path", path}});
            continue;
        }
        // Never export into a resource folder: the copy would recurse into itself.
        if (isPathInside(source, root)) {
            code = "UNSAFE_DESTINATION";
            error = "导出目录不能位于资源目录内部: " + path;
            return json::object();
        }
        const std::string safeName = sanitizeFileName(row.value("name", "resource"));
        const std::string relative = type + "/" + safeName + "-" + id.substr(4, 8);
        json item{{"id", id},
                  {"type", type},
                  {"name", row.value("name", "")},
                  {"version", row.value("version", "")},
                  {"sourcePath", path},
                  {"relativePath", relative},
                  {"size", row.value("size", 0ull)},
                  {"fileCount", row.value("file_count", 0)},
                  {"sha256", row.value("sha256", "")},
                  {"repoId", row.value("repo_id", "")},
                  {"revision", row.value("revision", "")},
                  {"source", row.value("source", "")}};
        uint64_t bytes = 0;
        int files = 0;
        directoryUsage(source, bytes, files);
        if (fs::is_regular_file(source, ec)) {
            bytes = static_cast<uint64_t>(fs::file_size(source, ec));
            files = 1;
        }
        item["size"] = bytes;
        item["fileCount"] = files;
        if (includeSha && fs::is_regular_file(source, ec)) {
            const std::string digest = sha256File(source);
            item["sha256"] = digest;
            sums += digest + "  " + relative + "\n";
        }
        if (copyFiles) {
            const fs::path target = root / "files" / pathFromUtf8(relative);
            std::string copyError;
            if (!copyTree(source, target, copiedBytes, copyError)) {
                code = "COPY_FAILED";
                error = copyError;
                return json::object();
            }
            if (includeSha && fs::is_directory(target, ec)) {
                // Directory resources (models/datasets) get per-file checksums so the
                // receiving side can verify every shard, not just a folder name.
                for (fs::recursive_directory_iterator it(target, ec), end; it != end; it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec)) continue;
                    const std::string fileRel = "files/" + relative + "/" +
                                                pathToUtf8(it->path().lexically_relative(target));
                    sums += sha256File(it->path()) + "  " + fileRel + "\n";
                }
            }
            ++copied;
        }
        totalSize += bytes;
        manifestItems.push_back(item);
    }

    if (manifestItems.empty()) {
        code = "NO_RESOURCES";
        error = "没有可导出的离线资源（可能全部缺失或筛选条件过窄）";
        return json::object();
    }

    json manifest{{"format", kManifestFormat},
                  {"name", options.value("name", "ModelForge 离线资源包")},
                  {"createdAt", isoNow()},
                  {"application", std::string("ModelForge ") + MODELEFORGE_VERSION},
                  {"platform", getOsName()},
                  {"copyFiles", copyFiles},
                  {"totalSize", totalSize},
                  {"itemCount", static_cast<int>(manifestItems.size())},
                  {"items", manifestItems},
                  {"missing", missing}};
    const fs::path manifestPath = root / kManifestName;
    if (!writeTextFileAtomic(manifestPath, manifest.dump(2))) {
        code = "MANIFEST_WRITE_FAILED";
        error = "无法写入清单文件: " + pathToUtf8(manifestPath);
        return json::object();
    }
    if (!sums.empty()) writeTextFileAtomic(root / kSumsName, sums);

    db_.query("INSERT INTO offline_bundles(id,name,root,manifest_path,item_count,total_size,direction,created_at) "
              "VALUES(?,?,?,?,?,?,'export',?)",
              {newId("bundle"), manifest.value("name", ""), pathToUtf8(root), pathToUtf8(manifestPath),
               static_cast<int>(manifestItems.size()), totalSize, isoNow()});
    Logger::instance().info("[offline-library] 导出清单 " + pathToUtf8(manifestPath) + "（" +
                            std::to_string(manifestItems.size()) + " 项）");
    return json{{"manifestPath", pathToUtf8(manifestPath)},
                {"root", pathToUtf8(root)},
                {"itemCount", static_cast<int>(manifestItems.size())},
                {"totalSize", totalSize},
                {"copied", copied},
                {"copiedBytes", copiedBytes},
                {"missing", missing},
                {"copyFiles", copyFiles}};
}

json OfflineLibrary::importBundle(const std::string& manifestInput, const json& options, std::string& code,
                                  std::string& error) {
    if (trim(manifestInput).empty()) {
        code = "INVALID_MANIFEST";
        error = "请提供离线资源包目录或清单文件路径";
        return json::object();
    }
    fs::path manifestPath = pathFromUtf8(trim(manifestInput));
    std::error_code ec;
    if (fs::is_directory(manifestPath, ec)) manifestPath /= kManifestName;
    if (!fs::is_regular_file(manifestPath, ec)) {
        code = "MANIFEST_NOT_FOUND";
        error = "清单文件不存在: " + pathToUtf8(manifestPath);
        return json::object();
    }
    json manifest;
    try {
        manifest = readJsonFile(manifestPath);
    } catch (const std::exception& ex) {
        code = "INVALID_MANIFEST";
        error = std::string("清单文件无法解析: ") + ex.what();
        return json::object();
    }
    if (!manifest.is_object() || manifest.value("format", "") != kManifestFormat) {
        code = "INVALID_MANIFEST";
        error = "清单格式不受支持，需要 " + std::string(kManifestFormat);
        return json::object();
    }
    const std::string baseInput = trim(options.value("basePath", ""));
    const fs::path base = baseInput.empty() ? fs::path() : fs::absolute(pathFromUtf8(baseInput));
    const bool verifySha = options.value("verifySha256", false);
    const fs::path manifestRoot = manifestPath.parent_path();

    json imported = json::array();
    json missing = json::array();
    json mismatched = json::array();
    int index = 0;
    for (const auto& item : manifest.value("items", json::array())) {
        if (!item.is_object()) continue;
        const std::string relative = item.value("relativePath", "");
        fs::path candidate;
        if (!base.empty()) {
            // Bundles created with copy=true keep files under <root>/files/<relative>.
            const fs::path copied = base / "files" / pathFromUtf8(relative);
            candidate = fs::exists(copied, ec) ? copied : (base / pathFromUtf8(relative));
        } else if (relative.empty()) {
            candidate = pathFromUtf8(item.value("sourcePath", ""));
        } else {
            const fs::path local = manifestRoot / "files" / pathFromUtf8(relative);
            candidate = fs::exists(local, ec) ? local : pathFromUtf8(item.value("sourcePath", ""));
        }
        if (candidate.empty() || !fs::exists(candidate, ec)) {
            missing.push_back(json{{"name", item.value("name", "")}, {"expected", pathToUtf8(candidate)}});
            continue;
        }
        if (verifySha) {
            const std::string expected = item.value("sha256", "");
            if (!expected.empty() && fs::is_regular_file(candidate, ec)) {
                const std::string actual = toLower(sha256File(candidate));
                if (actual != toLower(expected)) {
                    mismatched.push_back(json{{"name", item.value("name", "")}, {"path", pathToUtf8(candidate)},
                                              {"expected", expected}, {"actual", actual}});
                    continue;
                }
            }
        }
        uint64_t bytes = 0;
        int files = 0;
        directoryUsage(candidate, bytes, files);
        if (fs::is_regular_file(candidate, ec)) {
            bytes = static_cast<uint64_t>(fs::file_size(candidate, ec));
            files = 1;
        }
        json meta = item.value("meta", json::object());
        meta["importedFrom"] = pathToUtf8(manifestPath);
        meta["bundleName"] = manifest.value("name", "");
        const std::string id = upsertResource(item.value("type", "other"), item.value("name", ""),
                                             item.value("version", ""), pathToUtf8(fs::absolute(candidate, ec)),
                                             bytes, files, item.value("sha256", ""), "bundle",
                                             item.value("repoId", ""), item.value("revision", ""), meta);
        imported.push_back(json{{"id", id}, {"name", item.value("name", "")}, {"path", pathToUtf8(candidate)}});
        ++index;
    }
    db_.query("INSERT INTO offline_bundles(id,name,root,manifest_path,item_count,total_size,direction,created_at) "
              "VALUES(?,?,?,?,?,?,'import',?)",
              {newId("bundle"), manifest.value("name", "离线资源包"), pathToUtf8(manifestRoot),
               pathToUtf8(manifestPath), index, static_cast<uint64_t>(manifest.value("totalSize", 0ull)), isoNow()});
    if (index == 0) {
        code = "NOTHING_IMPORTED";
        error = missing.empty() ? "清单中没有可导入的资源" : "清单中的资源文件全部缺失";
        return json{{"imported", imported}, {"missing", missing}, {"mismatched", mismatched}};
    }
    Logger::instance().info("[offline-library] 导入 " + pathToUtf8(manifestPath) + "，" + std::to_string(index) + " 项");
    return json{{"imported", imported},
                {"importedCount", index},
                {"missing", missing},
                {"mismatched", mismatched},
                {"manifestPath", pathToUtf8(manifestPath)}};
}

json OfflineLibrary::bundles(int limit) {
    limit = std::max(1, std::min(limit, 100));
    json items = json::array();
    for (const auto& row : db_.query("SELECT * FROM offline_bundles ORDER BY created_at DESC LIMIT ?", {limit})) {
        items.push_back(json{{"id", row.value("id", "")},
                             {"name", row.value("name", "")},
                             {"root", row.value("root", "")},
                             {"manifestPath", row.value("manifest_path", "")},
                             {"itemCount", row.value("item_count", 0)},
                             {"totalSize", row.value("total_size", 0ull)},
                             {"direction", row.value("direction", "")},
                             {"createdAt", row.value("created_at", "")}});
    }
    return items;
}

bool OfflineLibrary::forget(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT id FROM offline_resources WHERE id=?", {id});
    if (rows.empty()) {
        error = "资源不存在: " + id;
        return false;
    }
    db_.query("DELETE FROM offline_resources WHERE id=?", {id});
    return true;
}

json OfflineLibrary::verifyAll() {
    json missing = json::array();
    json drifted = json::array();
    int checked = 0;
    for (const auto& row : db_.query("SELECT * FROM offline_resources")) {
        const json item = resourceRow(row);
        ++checked;
        if (!item.value("present", false)) {
            missing.push_back(json{{"id", item["id"]}, {"name", item["name"]}, {"path", item["path"]}});
            continue;
        }
        if (!item.value("sizeMatches", true)) {
            drifted.push_back(json{{"id", item["id"]}, {"name", item["name"]},
                                   {"expected", item["size"]}, {"actual", item["onDiskBytes"]}});
        }
    }
    return json{{"checked", checked}, {"missing", missing}, {"sizeDrift", drifted},
                {"healthy", checked - static_cast<int>(missing.size())}};
}

} // namespace mf
