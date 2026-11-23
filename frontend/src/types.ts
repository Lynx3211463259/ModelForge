export interface Envelope<T> {
  ok: boolean
  data: T
  error?: { code: string; message: string; suggestion?: string; details?: unknown }
}

export interface Task {
  id: string
  modelId: string
  modelName: string
  status: string
  stage: string
  stageLabel?: string
  stageDetail?: string
  progress: number
  progressMode: 'measured' | 'estimated' | 'stage' | 'indeterminate'
  priority: number
  inputPath: string
  outputPath: string
  outputSize: number
  createdAt: number
  startedAt: number
  finishedAt: number
  elapsedMs: number
  errorCode: string
  errorMessage: string
  suggestion?: string
  sha256: string
  processedBytes?: number
  totalBytes?: number
  params: Record<string, any>
  verification?: Record<string, any>
  resources?: TaskResources
}

export interface TaskResourceStage {
  stage: string
  stageLabel: string
  cpuMs: number
  durationMs: number
  peakMemory: number
  readBytes: number
  writeBytes: number
  samples: number
  updatedAt: string
}

export interface TaskResources {
  stages: TaskResourceStage[]
  cpuMs: number
  durationMs: number
  peakMemory: number
  readBytes: number
  writeBytes: number
  cpuPercent: number
  sampled: boolean
}

export interface Model {
  id: string
  name: string
  path: string
  architecture: string
  architectureFamily: string
  parameterCount: number
  dtype: string
  totalSize: number
  shardCount: number
  tensorCount: number
  status: string
  supported: boolean
  favorite: boolean
  notes: string
  tags: string
  createdAt: string
  updatedAt: string
  analysis?: any
  files?: Array<Record<string, any>>
  versions?: Array<Record<string, any>>
  compatibility?: { supported: boolean; reason: string; match?: string }
  warnings?: string[]
  errors?: string[]
}

export interface Capabilities {
  pythonAvailable: boolean
  pythonDependenciesReady: boolean
  convertScriptAvailable: boolean
  quantizeAvailable: boolean
  cliAvailable: boolean
  serverAvailable: boolean
  benchAvailable: boolean
  pplAvailable: boolean
  pplPath: string
  pythonVersion: string
  pythonDependencyError: string
  convertScriptVersion: string
  quantizeVersion: string
  quantizePath: string
  convertScriptPath: string
  cliPath: string
  benchPath: string
  quantTypes: string[]
  conversionTypes: string[]
  supportedArchitectures: string[]
  issues: string[]
  conversionReady: boolean
  ready: boolean
}

export interface EstimateOption {
  type: string
  bitsPerWeight: number
  estimatedSize: number
  estimatedRuntimeMemory: number
  available: boolean
  reason: string
  description: string
}

export interface LogEntry {
  seq: number
  time: string
  level: 'INFO' | 'WARNING' | 'ERROR' | 'DEBUG' | string
  message: string
}

export interface ResourceInfo {
  cpuPercent: number
  memoryTotal: number
  memoryAvailable: number
  memoryUsed: number
  memoryPercent: number
  diskFree: number
  diskTotal: number
  gpu: string
  os: string
  host: string
  threads: number
}

export interface Settings {
  pythonPath: string
  convertScript: string
  quantizePath: string
  llamaCliPath: string
  llamaServerPath: string
  llamaBenchPath: string
  llamaPplPath: string
  pplDataset: string
  llamaRoot: string
  llamaBackend: string
  llamaChannel: string
  llamaRepo: string
  defaultModelDir: string
  defaultOutputDir: string
  tempDir: string
  dataDir: string
  webRoot: string
  bindAddress: string
  port: number
  threads: number
  maxConcurrent: number
  autoOpenBrowser: boolean
  logTimestamps: boolean
  theme: string
  language: string
  updateChannel: string
  folderWatchEnabled: boolean
  watchFolders: string[]
  watchIntervalSec: number
  watchAutoConvert: boolean
  watchPresetId: string
}


export interface UpdateInfo {
  currentVersion: string
  latestVersion: string
  releaseName: string
  releaseNotes: string
  htmlUrl: string
  publishedAt: string
  updateAvailable: boolean
  prerelease: boolean
  channel: string
  assetName: string
  assetSize: number
  assetSizeLabel: string
  code: string
  message: string
  suggestion: string
}

export interface UpdateJob {
  id: string
  status: string
  statusLabel: string
  progress: number
  message: string
  code: string
  version: string
  assetName: string
  downloadedBytes: number
  totalBytes: number
  scriptPath: string
}

export interface ModelDeleteResult {
  modelId: string
  modelName: string
  deleteFiles: boolean
  removedFiles: number
  freedBytes: number
  freedLabel: string
  missingFiles: string[]
  sharedFiles: string[]
  unsafeFiles: string[]
  keptArtifacts: string[]
  warnings: string[]
}
