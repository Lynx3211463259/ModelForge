-- ModelForge SQLite schema (bootstrap, applied at startup)
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

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (2, datetime('now'));

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES (1, datetime('now'));
