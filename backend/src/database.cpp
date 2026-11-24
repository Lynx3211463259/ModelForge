#include "modelforge/database.hpp"

#include "modelforge/util.hpp"

#include <cstring>
#include <functional>
#include <stdexcept>

namespace mf {

Database::~Database() { close(); }

bool Database::open(const std::string& path, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = path;
        const int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            error = db_ ? sqlite3_errmsg(db_) : "无法打开 SQLite 数据库";
            if (db_) { sqlite3_close(db_); db_ = nullptr; }
            return false;
        }
        sqlite3_busy_timeout(db_, 5000);
        char* message = nullptr;
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL;", nullptr, nullptr, &message);
        if (message) sqlite3_free(message);
    }
    try {
        migrate();
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    return true;
}

void Database::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

std::string Database::lastError() const { return lastError_; }

int Database::changes() {
    std::lock_guard<std::mutex> lock(mutex_);
    return db_ ? sqlite3_changes(db_) : 0;
}

bool Database::exec(const std::string& sql, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) { error = "数据库未初始化"; return false; }
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        error = message ? message : sqlite3_errmsg(db_);
        lastError_ = error;
        if (message) sqlite3_free(message);
        return false;
    }
    if (message) sqlite3_free(message);
    return true;
}

json Database::query(const std::string& sql, const std::vector<json>& params, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    json rows = json::array();
    if (!db_) {
        if (error) *error = "数据库未初始化";
        return rows;
    }
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        if (error) *error = sqlite3_errmsg(db_);
        lastError_ = error ? *error : "SQL prepare failed";
        return rows;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& value = params[i];
        const int index = static_cast<int>(i + 1);
        if (value.is_null()) sqlite3_bind_null(stmt, index);
        else if (value.is_boolean()) sqlite3_bind_int(stmt, index, value.get<bool>() ? 1 : 0);
        else if (value.is_number_integer()) sqlite3_bind_int64(stmt, index, value.get<int64_t>());
        else if (value.is_number_unsigned()) sqlite3_bind_int64(stmt, index, static_cast<int64_t>(value.get<uint64_t>()));
        else if (value.is_number_float()) sqlite3_bind_double(stmt, index, value.get<double>());
        else if (value.is_string()) {
            const auto text = value.get<std::string>();
            sqlite3_bind_text(stmt, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
        } else {
            const auto text = value.dump();
            sqlite3_bind_text(stmt, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
        }
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        json row = json::object();
        const int columns = sqlite3_column_count(stmt);
        for (int c = 0; c < columns; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            const int type = sqlite3_column_type(stmt, c);
            if (type == SQLITE_NULL) row[name] = "";
            else if (type == SQLITE_INTEGER) row[name] = static_cast<int64_t>(sqlite3_column_int64(stmt, c));
            else if (type == SQLITE_FLOAT) row[name] = sqlite3_column_double(stmt, c);
            else {
                const unsigned char* text = sqlite3_column_text(stmt, c);
                const int bytes = sqlite3_column_bytes(stmt, c);
                row[name] = text ? std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes)) : "";
            }
        }
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE && error) *error = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return rows;
}

void Database::transaction(const std::function<void()>& fn) {
    std::string ignored;
    exec("BEGIN IMMEDIATE", ignored);
    try {
        fn();
        exec("COMMIT", ignored);
    } catch (...) {
        exec("ROLLBACK", ignored);
        throw;
    }
}

void Database::migrate() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
  version INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS models (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  path TEXT NOT NULL,
  architecture TEXT,
  architecture_family TEXT,
  parameter_count INTEGER DEFAULT 0,
  dtype TEXT,
  total_size INTEGER DEFAULT 0,
  shard_count INTEGER DEFAULT 0,
  tensor_count INTEGER DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'READY',
  supported INTEGER DEFAULT 0,
  favorite INTEGER DEFAULT 0,
  cover TEXT,
  notes TEXT DEFAULT '',
  tags TEXT DEFAULT '',
  analysis_json TEXT,
  config_json TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS model_files (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  model_id TEXT NOT NULL REFERENCES models(id) ON DELETE CASCADE,
  path TEXT NOT NULL,
  file_name TEXT NOT NULL,
  size INTEGER DEFAULT 0,
  kind TEXT,
  sha256 TEXT,
  valid INTEGER DEFAULT 1,
  details_json TEXT
);
CREATE INDEX IF NOT EXISTS idx_model_files_model ON model_files(model_id);
CREATE INDEX IF NOT EXISTS idx_models_created ON models(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_models_name ON models(name);

CREATE TABLE IF NOT EXISTS conversion_tasks (
  id TEXT PRIMARY KEY,
  model_id TEXT,
  model_name TEXT NOT NULL,
  status TEXT NOT NULL,
  stage TEXT NOT NULL,
  progress INTEGER DEFAULT 0,
  progress_mode TEXT DEFAULT 'estimated',
  priority INTEGER DEFAULT 0,
  input_path TEXT NOT NULL,
  output_path TEXT,
  output_size INTEGER DEFAULT 0,
  params_json TEXT NOT NULL,
  error_code TEXT,
  error_message TEXT,
  logs_path TEXT,
  sha256 TEXT,
  verification_json TEXT,
  queue_position INTEGER DEFAULT 0,
  created_at INTEGER NOT NULL,
  started_at INTEGER DEFAULT 0,
  finished_at INTEGER DEFAULT 0,
  elapsed_ms INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_tasks_status ON conversion_tasks(status, priority DESC, created_at ASC);
CREATE INDEX IF NOT EXISTS idx_tasks_created ON conversion_tasks(created_at DESC);

CREATE TABLE IF NOT EXISTS task_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL REFERENCES conversion_tasks(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL,
  level TEXT NOT NULL,
  message TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_task_logs_task ON task_logs(task_id, seq);

CREATE TABLE IF NOT EXISTS quantization_profiles (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  description TEXT,
  params_json TEXT NOT NULL,
  builtin INTEGER DEFAULT 0,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS conversion_history (
  id TEXT PRIMARY KEY,
  model_id TEXT,
  task_id TEXT,
  event TEXT NOT NULL,
  detail TEXT,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_history_created ON conversion_history(created_at DESC);

CREATE TABLE IF NOT EXISTS model_versions (
  id TEXT PRIMARY KEY,
  model_id TEXT NOT NULL,
  task_id TEXT,
  quant_type TEXT,
  path TEXT NOT NULL,
  size INTEGER DEFAULT 0,
  sha256 TEXT,
  tool_version TEXT,
  parent_id TEXT,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_versions_model ON model_versions(model_id, created_at DESC);

CREATE TABLE IF NOT EXISTS workflows (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  description TEXT,
  definition_json TEXT NOT NULL,
  enabled INTEGER DEFAULT 1,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS workflow_steps (
  id TEXT PRIMARY KEY,
  workflow_id TEXT NOT NULL REFERENCES workflows(id) ON DELETE CASCADE,
  step_order INTEGER NOT NULL,
  type TEXT NOT NULL,
  config_json TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS benchmark_results (
  id TEXT PRIMARY KEY,
  model_id TEXT,
  version_id TEXT,
  quant_type TEXT,
  metrics_json TEXT NOT NULL,
  environment_json TEXT,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS app_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS remote_nodes (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  endpoint TEXT NOT NULL,
  status TEXT DEFAULT 'OFFLINE',
  capabilities_json TEXT,
  last_seen TEXT,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS downloads (
  id TEXT PRIMARY KEY,
  repo_id TEXT NOT NULL,
  target_dir TEXT NOT NULL,
  status TEXT NOT NULL,
  progress INTEGER DEFAULT 0,
  total_bytes INTEGER DEFAULT 0,
  downloaded_bytes INTEGER DEFAULT 0,
  error_message TEXT,
  created_at INTEGER NOT NULL,
  finished_at INTEGER DEFAULT 0
);

CREATE TABLE IF NOT EXISTS hf_repositories (
  id TEXT PRIMARY KEY,
  repo_id TEXT NOT NULL,
  revision TEXT NOT NULL,
  commit_hash TEXT,
  repo_type TEXT DEFAULT 'model',
  total_size INTEGER DEFAULT 0,
  file_count INTEGER DEFAULT 0,
  license TEXT,
  gated INTEGER DEFAULT 0,
  raw_json TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_hf_repos_repo ON hf_repositories(repo_id, revision);

CREATE TABLE IF NOT EXISTS download_tasks (
  id TEXT PRIMARY KEY,
  repository_id TEXT,
  repo_id TEXT NOT NULL,
  revision TEXT NOT NULL,
  commit_hash TEXT,
  destination TEXT NOT NULL,
  status TEXT NOT NULL,
  total_bytes INTEGER DEFAULT 0,
  downloaded_bytes INTEGER DEFAULT 0,
  progress INTEGER DEFAULT 0,
  speed REAL DEFAULT 0,
  concurrency INTEGER DEFAULT 4,
  max_speed INTEGER DEFAULT 0,
  retry_count INTEGER DEFAULT 0,
  file_count INTEGER DEFAULT 0,
  completed_files INTEGER DEFAULT 0,
  current_files TEXT DEFAULT '',
  selection_json TEXT,
  error_message TEXT,
  result_json TEXT,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  completed_at INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_download_tasks_status ON download_tasks(status, created_at DESC);

CREATE TABLE IF NOT EXISTS download_files (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL REFERENCES download_tasks(id) ON DELETE CASCADE,
  remote_path TEXT NOT NULL,
  local_path TEXT NOT NULL,
  file_size INTEGER DEFAULT 0,
  downloaded_size INTEGER DEFAULT 0,
  etag TEXT,
  last_modified TEXT,
  checksum TEXT,
  sha256 TEXT,
  status TEXT NOT NULL DEFAULT 'pending',
  retry_count INTEGER DEFAULT 0,
  error_message TEXT,
  updated_at INTEGER NOT NULL,
  UNIQUE(task_id, remote_path)
);
CREATE INDEX IF NOT EXISTS idx_download_files_task ON download_files(task_id);

CREATE TABLE IF NOT EXISTS download_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL,
  seq INTEGER NOT NULL,
  level TEXT NOT NULL,
  message TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_download_logs_task ON download_logs(task_id, seq);

CREATE TABLE IF NOT EXISTS download_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS task_resources (
  task_id TEXT NOT NULL,
  stage TEXT NOT NULL,
  cpu_ms INTEGER NOT NULL DEFAULT 0,
  peak_memory INTEGER NOT NULL DEFAULT 0,
  read_bytes INTEGER NOT NULL DEFAULT 0,
  write_bytes INTEGER NOT NULL DEFAULT 0,
  duration_ms INTEGER NOT NULL DEFAULT 0,
  samples INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL,
  PRIMARY KEY (task_id, stage)
);

CREATE INDEX IF NOT EXISTS idx_task_resources_task ON task_resources(task_id);

CREATE TABLE IF NOT EXISTS ppl_results (
  id TEXT PRIMARY KEY,
  model_id TEXT NOT NULL,
  version_id TEXT NOT NULL DEFAULT '',
  quant_type TEXT NOT NULL DEFAULT '',
  gguf_path TEXT NOT NULL,
  dataset_path TEXT NOT NULL DEFAULT '',
  dataset_kind TEXT NOT NULL DEFAULT 'custom',
  dataset_bytes INTEGER NOT NULL DEFAULT 0,
  context INTEGER NOT NULL DEFAULT 0,
  chunks INTEGER NOT NULL DEFAULT 0,
  tokens INTEGER NOT NULL DEFAULT 0,
  ppl REAL NOT NULL DEFAULT 0,
  ppl_stderr REAL NOT NULL DEFAULT 0,
  duration_ms INTEGER NOT NULL DEFAULT 0,
  cpu_ms INTEGER NOT NULL DEFAULT 0,
  peak_memory INTEGER NOT NULL DEFAULT 0,
  raw_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_ppl_model ON ppl_results(model_id, created_at DESC);

CREATE TABLE IF NOT EXISTS watch_seen (
  path TEXT PRIMARY KEY,
  signature TEXT NOT NULL,
  model_id TEXT NOT NULL DEFAULT '',
  task_id TEXT NOT NULL DEFAULT '',
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS watch_events (
  id TEXT PRIMARY KEY,
  folder TEXT NOT NULL DEFAULT '',
  path TEXT NOT NULL,
  action TEXT NOT NULL,
  model_id TEXT NOT NULL DEFAULT '',
  task_id TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_watch_events_created ON watch_events(created_at DESC);

-- Offline resource library: every artifact ModelForge can serve without network.
-- Rows only describe files on disk; unregistering never deletes the files.
CREATE TABLE IF NOT EXISTS offline_resources (
  id TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  name TEXT NOT NULL,
  version TEXT NOT NULL DEFAULT '',
  path TEXT NOT NULL,
  size INTEGER DEFAULT 0,
  file_count INTEGER DEFAULT 0,
  sha256 TEXT DEFAULT '',
  source TEXT NOT NULL DEFAULT '',
  repo_id TEXT NOT NULL DEFAULT '',
  revision TEXT NOT NULL DEFAULT '',
  meta_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_offline_type ON offline_resources(type, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_offline_path ON offline_resources(path);

-- Bundle history for export/import so the UI can explain where a bundle came from.
CREATE TABLE IF NOT EXISTS offline_bundles (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  root TEXT NOT NULL,
  manifest_path TEXT NOT NULL,
  item_count INTEGER DEFAULT 0,
  total_size INTEGER DEFAULT 0,
  direction TEXT NOT NULL DEFAULT 'export',
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_offline_bundles_created ON offline_bundles(created_at DESC);

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (3, datetime('now'));

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (2, datetime('now'));

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (1, datetime('now'));
)SQL";
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string error = message ? message : "数据库迁移失败";
        if (message) sqlite3_free(message);
        throw std::runtime_error(error);
    }
    if (message) sqlite3_free(message);

    // Additive column migrations for databases created by earlier versions.
    // SQLite has no "ADD COLUMN IF NOT EXISTS", so inspect PRAGMA table_info first.
    auto addColumn = [this](const char* table, const char* column, const char* definition) {
        const auto info = query(std::string("PRAGMA table_info(") + table + ")");
        for (const auto& row : info) {
            if (row.value("name", "") == column) return;
        }
        std::string ignored;
        exec(std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + definition, ignored);
    };
    addColumn("download_tasks", "repo_type", "TEXT NOT NULL DEFAULT 'model'");

    // Seed built-in profiles only when the table is empty.
    const auto countRows = query("SELECT COUNT(*) AS n FROM quantization_profiles");
    if (!countRows.empty() && countRows[0].value("n", 0) == 0) {
        struct Seed { const char* id; const char* name; const char* desc; const char* params; };
        const Seed seeds[] = {
            {"balanced", "平衡模式", "优先 Q4_K_M，在体积与质量之间取得平衡", R"({"outType":"f16","quantType":"Q4_K_M","threads":0,"verify":true})"},
            {"quality", "质量优先", "使用 Q6_K，保留更多模型质量", R"({"outType":"f16","quantType":"Q6_K","threads":0,"verify":true})"},
            {"performance", "高性能模式", "使用 Q5_K_M，适合较充足的显存", R"({"outType":"f16","quantType":"Q5_K_M","threads":0,"verify":true})"},
            {"light", "轻量模式", "使用 Q3_K_M，降低内存占用", R"({"outType":"f16","quantType":"Q3_K_M","threads":0,"verify":true})"},
            {"extreme", "极限压缩", "使用 Q2_K，质量损失较明显", R"({"outType":"f16","quantType":"Q2_K","threads":0,"verify":true})"}
        };
        for (const auto& seed : seeds) {
            query("INSERT INTO quantization_profiles(id,name,description,params_json,builtin,created_at) VALUES(?,?,?,?,1,?)",
                  {seed.id, seed.name, seed.desc, seed.params, isoNow()});
        }
    }
}

} // namespace mf
