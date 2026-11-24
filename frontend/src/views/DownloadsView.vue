<script setup lang="ts">
import { t } from '../i18n'
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api, humanBytes, formatDuration, formatTime } from '../api/client'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import ProgressBar from '../components/ProgressBar.vue'

interface HfFile {
  path: string
  size: number
  category: string
  sha256: string
  lfs?: boolean
}

interface HfRepo {
  repoId: string
  author: string
  name: string
  revision: string
  commitHash: string
  repoType: string
  license: string
  pipelineTag: string
  library: string
  totalSize: number
  fileCount: number
  gated: boolean
  private: boolean
  files: HfFile[]
  warning?: string
  tokenUsed?: boolean
}

interface DownloadTask {
  id: string
  repoId: string
  repoType?: string
  revision: string
  commitHash: string
  destination: string
  status: string
  statusLabel: string
  progress: number
  totalBytes: number
  downloadedBytes: number
  speed: number
  concurrency: number
  maxSpeed: number
  fileCount: number
  completedFiles: number
  currentFiles: string
  errorMessage: string
  createdAt: number
  updatedAt: number
  completedAt: number
  result: Record<string, any>
  files?: DownloadFile[]
}

interface DownloadFile {
  id: number
  remotePath: string
  localPath: string
  size: number
  downloaded: number
  status: string
  checksum: string
  retryCount: number
  error: string
}

const app = useAppStore()

const input = ref('')
// model | dataset: datasets use the /api/datasets endpoints and land in datasetsDir.
const kind = ref<'model' | 'dataset'>('model')
const revision = ref('main')
const destination = ref('')
const destinationTouched = ref(false)
const concurrency = ref(4)
const maxSpeedMb = ref(0)
const verifyChecksum = ref(true)
const autoImport = ref(true)
const tokenInput = ref('')
const tokenStatus = ref<{ configured: boolean; masked: string; scheme: string }>({ configured: false, masked: '', scheme: '' })
const tokenCheck = ref<Record<string, any> | null>(null)

const resolving = ref(false)
const resolved = ref<HfRepo | null>(null)
const resolveError = ref('')
const selectedPaths = ref<string[]>([])
const fileSearch = ref('')
const categoryFilter = ref('all')
const includePattern = ref('')
const excludePattern = ref('')
const filesExpanded = ref(true)

const tasks = ref<DownloadTask[]>([])
const settings = ref<Record<string, any>>({})
const engineRunning = ref(false)
const loadingTasks = ref(false)
const selectedTaskId = ref('')
const detailTask = ref<DownloadTask | null>(null)
const detailFiles = ref<DownloadFile[]>([])
const detailLogs = ref<Array<{ seq: number; time: string; level: string; message: string }>>([])
const lastLogSeq = ref(0)
const busy = ref('')

const categoryLabels: Record<string, string> = {
  safetensors: 'Safetensors',
  pytorch: 'PyTorch',
  gguf: 'GGUF',
  config: t('配置'),
  tokenizer: 'Tokenizer',
  vocab: t('词表'),
  index: t('分片索引'),
  readme: t('说明'),
  parquet: 'Parquet',
  arrow: 'Arrow',
  csv: 'CSV',
  jsonl: 'JSON Lines',
  json: 'JSON',
  text: t('纯文本'),
  image: t('图像'),
  audio: t('音频'),
  video: t('视频'),
  archive: t('压缩包'),
  tensor: t('张量数据'),
  other: t('其他')
}

const categories = computed(() => {
  const set = new Set<string>()
  for (const file of resolved.value?.files || []) set.add(file.category)
  return ['all', ...Array.from(set)]
})

const visibleFiles = computed(() => {
  const needle = fileSearch.value.trim().toLowerCase()
  const include = includePattern.value.trim().toLowerCase()
  const exclude = excludePattern.value.trim().toLowerCase()
  return (resolved.value?.files || []).filter((file) => {
    if (categoryFilter.value !== 'all' && file.category !== categoryFilter.value) return false
    if (needle && !file.path.toLowerCase().includes(needle)) return false
    if (include && !globMatch(include, file.path.toLowerCase())) return false
    if (exclude && globMatch(exclude, file.path.toLowerCase())) return false
    return true
  })
})

const selectedFiles = computed(() => (resolved.value?.files || []).filter((file) => selectedPaths.value.includes(file.path)))
const selectedBytes = computed(() => selectedFiles.value.reduce((sum, file) => sum + (file.size || 0), 0))

const activeTasks = computed(() => tasks.value.filter((task) => ['pending', 'downloading', 'retrying', 'verifying', 'resolving'].includes(task.status)))
const detailLogEntries = computed(() => detailLogs.value.map((entry) => ({
  seq: entry.seq,
  time: entry.time,
  level: entry.level,
  message: entry.message
})))

function globMatch(pattern: string, value: string): boolean {
  if (!pattern) return true
  let p = 0
  let v = 0
  let star = -1
  let mark = 0
  while (v < value.length) {
    if (p < pattern.length && (pattern[p] === '?' || pattern[p] === value[v])) {
      p++
      v++
    } else if (p < pattern.length && pattern[p] === '*') {
      star = p++
      mark = v
    } else if (star >= 0) {
      p = star + 1
      v = ++mark
    } else {
      return false
    }
  }
  while (p < pattern.length && pattern[p] === '*') p++
  return p === pattern.length
}

function categoryBadge(category: string) {
  if (category === 'safetensors' || category === 'gguf') return 'success'
  if (category === 'pytorch') return 'info'
  if (category === 'config' || category === 'index') return 'warning'
  return 'muted'
}

function formatEta(task: DownloadTask): string {
  if (!task.speed || task.speed <= 0) return '—'
  const remaining = Math.max(0, task.totalBytes - task.downloadedBytes)
  return formatDuration(Math.round((remaining / task.speed) * 1000))
}

function taskTone(status: string): string {
  if (status === 'completed') return 'success'
  if (status === 'failed') return 'danger'
  if (status === 'downloading' || status === 'retrying' || status === 'verifying') return 'active'
  if (status === 'paused' || status === 'pending') return 'warning'
  if (status === 'cancelled') return 'muted'
  return 'info'
}

async function loadSettings() {
  try {
    const data = await api.get<Record<string, any>>('/api/v1/downloads/settings')
    settings.value = data
    if (!destination.value) destination.value = data.defaultDestination || app.settings?.defaultModelDir || ''
    if (data.concurrency) concurrency.value = data.concurrency
    if (data.maxSpeed) maxSpeedMb.value = Math.round((data.maxSpeed / (1024 * 1024)) * 10) / 10
    verifyChecksum.value = data.verifyChecksum !== false
    autoImport.value = data.autoImport !== false
    tokenStatus.value = await api.get<{ configured: boolean; masked: string; scheme: string }>('/api/v1/huggingface/token')
  } catch (error) {
    app.fromError(error, t('读取下载设置失败'))
  }
}

async function loadTasks() {
  loadingTasks.value = true
  try {
    const data = await api.get<{ items: DownloadTask[]; settings: Record<string, any>; engineRunning: boolean }>('/api/v1/downloads?limit=100')
    tasks.value = data.items
    settings.value = { ...settings.value, ...data.settings }
    engineRunning.value = data.engineRunning
    if (selectedTaskId.value) await refreshDetail()
  } catch (error) {
    app.fromError(error, t('读取下载任务失败'))
  } finally {
    loadingTasks.value = false
  }
}

async function resolveRepo() {
  if (!input.value.trim()) {
    app.toast('warning', t('请输入仓库地址或 ID'), t('例如 Qwen/Qwen2.5-0.5B-Instruct，或完整 huggingface.co 链接。'))
    return
  }
  resolving.value = true
  resolveError.value = ''
  resolved.value = null
  selectedPaths.value = []
  try {
    const repo = await api.post<HfRepo>('/api/v1/huggingface/resolve', {
      input: input.value.trim(),
      revision: revision.value.trim() || 'main',
      repoType: kind.value,
      token: tokenInput.value.trim()
    })
    resolved.value = repo
    revision.value = repo.revision
    const defaultCategories = repo.repoType === 'dataset'
      ? ['parquet', 'arrow', 'csv', 'jsonl', 'json', 'text', 'image', 'audio', 'video', 'archive', 'tensor']
      : ['safetensors', 'pytorch', 'gguf', 'config', 'tokenizer', 'vocab', 'index']
    selectedPaths.value = repo.files
      .filter((file) => defaultCategories.includes(file.category))
      .map((file) => file.path)
    if (!selectedPaths.value.length) selectedPaths.value = repo.files.map((file) => file.path)
    if (!destinationTouched.value || !destination.value) {
      const base = String(
        repo.repoType === 'dataset'
          ? settings.value.datasetDestination || ''
          : settings.value.defaultDestination || app.settings?.defaultModelDir || ''
      ).replace(/\\/g, '/')
      destination.value = `${base.replace(/\/$/, '')}/${repo.name}`
      destinationTouched.value = false
    }
    if (repo.gated) app.toast('warning', t('该仓库需要许可协议'), t('请先在 huggingface.co 接受协议并配置 Token。'))
    if (repo.warning) app.toast('info', t('仓库提示'), repo.warning)
    app.toast('success', t('仓库解析成功'), t('{0} 个文件 · {1}', [repo.fileCount, humanBytes(repo.totalSize)]))
  } catch (error: any) {
    resolveError.value = error?.message || String(error)
    app.fromError(error, t('仓库解析失败'))
  } finally {
    resolving.value = false
  }
}

function toggleFile(path: string) {
  const index = selectedPaths.value.indexOf(path)
  if (index >= 0) selectedPaths.value.splice(index, 1)
  else selectedPaths.value.push(path)
}

function selectVisible() {
  const set = new Set(selectedPaths.value)
  for (const file of visibleFiles.value) set.add(file.path)
  selectedPaths.value = Array.from(set)
}

function invertVisible() {
  const set = new Set(selectedPaths.value)
  for (const file of visibleFiles.value) {
    if (set.has(file.path)) set.delete(file.path)
    else set.add(file.path)
  }
  selectedPaths.value = Array.from(set)
}

function selectWeights() {
  selectedPaths.value = (resolved.value?.files || [])
    .filter((file) => ['safetensors', 'pytorch', 'gguf', 'index'].includes(file.category))
    .map((file) => file.path)
}

function clearSelection() {
  selectedPaths.value = []
}

async function startDownload() {
  if (!resolved.value) return
  if (!selectedPaths.value.length) {
    app.toast('warning', t('请至少选择一个文件'))
    return
  }
  if (!destination.value.trim()) {
    app.toast('warning', t('请填写本地保存目录'))
    return
  }
  busy.value = 'create'
  try {
    const files = selectedFiles.value.map((file) => ({ path: file.path, size: file.size, sha256: file.sha256 }))
    const repo = resolved.value
    const task = await api.post<DownloadTask>('/api/v1/downloads', {
      repoId: repo.repoId,
      repoType: repo.repoType || kind.value,
      revision: repo.revision,
      commitHash: repo.commitHash,
      destination: destination.value.trim(),
      files,
      concurrency: concurrency.value,
      maxSpeed: Math.round((maxSpeedMb.value || 0) * 1024 * 1024),
      verifyChecksum: verifyChecksum.value,
      autoImport: autoImport.value,
      repository: {
        repoId: repo.repoId,
        revision: repo.revision,
        commitHash: repo.commitHash,
        license: repo.license,
        gated: repo.gated
      }
    })
    await api.put('/api/v1/downloads/settings', {
      concurrency: concurrency.value,
      maxSpeed: Math.round((maxSpeedMb.value || 0) * 1024 * 1024),
      verifyChecksum: verifyChecksum.value,
      autoImport: autoImport.value
    }).catch(() => undefined)
    app.toast('success', t('下载任务已创建'), t('{0} 个文件 · {1}', [selectedPaths.value.length, humanBytes(selectedBytes.value)]))
    await loadTasks()
    await selectTask(task.id)
  } catch (error) {
    app.fromError(error, t('创建下载任务失败'))
  } finally {
    busy.value = ''
  }
}

async function selectTask(id: string) {
  selectedTaskId.value = id
  lastLogSeq.value = 0
  detailLogs.value = []
  detailFiles.value = []
  await refreshDetail(true)
}

async function refreshDetail(reset = false) {
  if (!selectedTaskId.value) return
  try {
    const task = await api.get<DownloadTask>(`/api/v1/downloads/${selectedTaskId.value}`)
    detailTask.value = task
    detailFiles.value = task.files || detailFiles.value
    const logs = await api.get<{ items: any[]; lastSeq: number }>(
      `/api/v1/downloads/${selectedTaskId.value}/logs?after=${reset ? 0 : lastLogSeq.value}&limit=500`
    )
    if (reset) detailLogs.value = []
    for (const item of logs.items) {
      detailLogs.value.push(item)
      lastLogSeq.value = Math.max(lastLogSeq.value, item.seq)
    }
    if (detailLogs.value.length > 2000) detailLogs.value = detailLogs.value.slice(-2000)
  } catch (error) {
    // 404 after deletion is expected; keep the list usable.
  }
}

async function taskAction(task: DownloadTask, action: 'pause' | 'resume' | 'cancel' | 'retry' | 'delete') {
  if (action === 'delete' && !window.confirm(t('删除下载任务 {0} 的记录？已下载文件不会删除。', [task.id]))) return
  busy.value = action
  try {
    if (action === 'delete') await api.delete(`/api/v1/downloads/${task.id}`)
    else await api.post(`/api/v1/downloads/${task.id}/${action}`, {})
    if (action === 'delete' && selectedTaskId.value === task.id) {
      selectedTaskId.value = ''
      detailTask.value = null
      detailFiles.value = []
      detailLogs.value = []
    }
    const actionLabel = action === 'pause' ? t('暂停') : action === 'resume' ? t('继续')
      : action === 'cancel' ? t('取消') : action === 'retry' ? t('重试') : t('删除')
    app.toast('success', t('任务已{0}', [actionLabel]), task.id)
    await loadTasks()
  } catch (error) {
    app.fromError(error, t('操作失败'))
  } finally {
    busy.value = ''
  }
}

async function saveToken() {
  const value = tokenInput.value.trim()
  if (!value) {
    app.toast('warning', t('请输入 Token'))
    return
  }
  busy.value = 'token'
  try {
    const check = await api.post<Record<string, any>>('/api/v1/huggingface/token/validate', { token: value })
    tokenCheck.value = check
    if (!check.valid) {
      app.toast('error', t('Token 校验失败'), check.message || t('请检查 Token 权限'))
      return
    }
    tokenStatus.value = await api.put<{ configured: boolean; masked: string; scheme: string }>('/api/v1/huggingface/token', { token: value })
    tokenInput.value = ''
    app.toast('success', t('Token 已安全保存'), t('身份: {0}；已使用 DPAPI 加密存储。', [check.username || t('Hugging Face 用户')]))
  } catch (error) {
    app.fromError(error, t('Token 保存失败'))
  } finally {
    busy.value = ''
  }
}

async function clearToken() {
  try {
    tokenStatus.value = await api.put<{ configured: boolean; masked: string; scheme: string }>('/api/v1/huggingface/token', { token: '' })
    tokenCheck.value = null
    app.toast('success', t('Token 已清除'))
  } catch (error) {
    app.fromError(error, t('清除 Token 失败'))
  }
}

async function validateStoredToken() {
  busy.value = 'validate'
  try {
    tokenCheck.value = await api.post<Record<string, any>>('/api/v1/huggingface/token/validate', {})
    if (tokenCheck.value?.valid) app.toast('success', t('Token 有效'), t('身份: {0}', [tokenCheck.value.username || '—']))
    else app.toast('warning', t('Token 不可用'), tokenCheck.value?.message || t('未配置或已失效'))
  } catch (error) {
    app.fromError(error, t('Token 校验失败'))
  } finally {
    busy.value = ''
  }
}

function copyPath(path: string) {
  navigator.clipboard.writeText(path).then(
    () => app.toast('success', t('路径已复制')),
    () => app.toast('error', t('复制失败'))
  )
}

let pollTimer = 0
let eventTimer = 0
function scheduleRefresh() {
  if (eventTimer) return
  eventTimer = window.setTimeout(async () => {
    eventTimer = 0
    await loadTasks()
  }, 350)
}

function onRealtime(event: Event) {
  const payload = (event as CustomEvent).detail || {}
  if (typeof payload.type === 'string' && payload.type.startsWith('download.')) scheduleRefresh()
}

onMounted(async () => {
  await app.init()
  await Promise.all([loadSettings(), loadTasks()])
  pollTimer = window.setInterval(() => {
    if (activeTasks.value.length || selectedTaskId.value) loadTasks()
  }, 2000)
  window.addEventListener('mf:event', onRealtime)

})

onBeforeUnmount(() => {
  window.clearInterval(pollTimer)
  window.clearTimeout(eventTimer)
  window.removeEventListener('mf:event', onRealtime)
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('下载中心') }}</h1>
        <p class="page-subtitle">{{ t('通过 Hugging Face 官方 REST API 解析仓库，使用 WinHTTP 流式下载。进度来自真实写入字节数，断点保存在 .mfpart 文件中。') }}</p>
      </div>
      <div class="page-actions">
        <span class="badge" :class="engineRunning ? 'success' : 'warning'">{{ t('{0}', [engineRunning ? t('下载引擎运行中') : t('下载引擎未持有调度锁')]) }}</span>
        <button class="btn" :disabled="loadingTasks" @click="loadTasks"><Icon name="refresh" :size="13" /> {{ t('刷新') }}</button>
      </div>
    </div>

    <div class="dl-grid">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">{{ t('新建下载') }}</div>
            <div class="card-subtitle">{{ t('支持仓库 ID 与 huggingface.co 页面地址') }}</div>
          </div>
        </div>
        <div class="card-body">
          <div class="field">
            <label class="field-label">{{ t('资源类型') }}</label>
            <div class="segmented">
              <button class="segmented-item" :class="{ active: kind === 'model' }" @click="kind = 'model'">{{ t('模型') }}</button>
              <button class="segmented-item" :class="{ active: kind === 'dataset' }" @click="kind = 'dataset'">{{ t('数据集') }}</button>
            </div>
            <div class="field-hint">{{ t('数据集使用 Hugging Face datasets API，下载后登记到离线资源库，不会导入模型库。') }}</div>
          </div>
          <div class="field">
            <label class="field-label">{{ t('仓库地址 / 仓库 ID') }}</label>
            <input v-model="input" class="input mono" :placeholder="kind === 'dataset' ? 'tatsu-lab/alpaca' : 'Qwen/Qwen2.5-0.5B-Instruct'" @keyup.enter="resolveRepo" />
            <div class="field-hint">{{ t('也接受 https://huggingface.co/<owner>/<name> 与 /tree/<revision> 链接。') }}</div>
          </div>
          <div class="field-row">
            <div class="field">
              <label class="field-label">{{ t('分支 / Tag / Commit') }}</label>
              <input v-model="revision" class="input mono" placeholder="main" />
            </div>
            <div class="field">
              <label class="field-label">{{ t('并发文件数') }}</label>
              <input v-model.number="concurrency" type="number" min="1" max="16" class="input" />
            </div>
          </div>
          <div class="field">
            <label class="field-label">{{ t('本地保存目录') }}</label>
            <input v-model="destination" class="input mono" placeholder="C:\Users\me\models\Qwen2.5-0.5B" />
            <div class="field-hint">{{ t('文件按仓库相对路径写入；路径穿越与 Windows 保留名会在后端拒绝。') }}</div>
          </div>
          <div class="field-row">
            <div class="field">
              <label class="field-label">{{ t('限速（MB/s，0 为不限）') }}</label>
              <input v-model.number="maxSpeedMb" type="number" min="0" step="0.5" class="input" />
            </div>
            <div class="field">
              <label class="field-label">{{ t('下载后') }}</label>
              <label class="check-line"><input v-model="verifyChecksum" type="checkbox" /> {{ t('SHA256 校验') }}</label>
              <label class="check-line"><input v-model="autoImport" type="checkbox" /> {{ t('{0}', [kind === 'dataset' ? t('下载后登记离线资源库') : t('自动导入模型库')]) }}</label>
            </div>
          </div>

          <div class="section-label">{{ t('Hugging Face Token（私有仓库）') }}</div>
          <div class="token-row">
            <input v-model="tokenInput" type="password" autocomplete="off" class="input mono" placeholder="hf_..." />
            <button class="btn btn-sm" :disabled="busy === 'token'" @click="saveToken"><Icon name="save" :size="12" /> {{ t('校验并保存') }}</button>
          </div>
          <div class="field-hint">
            {{ t('已配置：') }}<span class="mono">{{ t('{0}', [tokenStatus.configured ? tokenStatus.masked : t('无')]) }}</span> {{ t('· 存储方式 {0}', [tokenStatus.scheme === 'dpapi' ? t('Windows DPAPI 加密') : t('本地数据库')]) }}<button class="btn btn-ghost btn-sm" :disabled="busy === 'validate'" @click="validateStoredToken">{{ t('验证') }}</button>
            <button v-if="tokenStatus.configured" class="btn btn-ghost btn-sm" @click="clearToken">{{ t('清除') }}</button>
          </div>
          <div v-if="tokenCheck?.valid" class="inline-ok"><Icon name="check" :size="12" /> {{ t('身份：{0}', [tokenCheck.username || t('已认证')]) }}</div>

          <div class="btn-row" style="margin-top: 14px">
            <button class="btn btn-primary" :disabled="resolving" @click="resolveRepo">
              <Icon name="search" :size="13" /> {{ t('{0}', [resolving ? t('正在解析…') : t('解析仓库')]) }}</button>
          </div>
          <div v-if="resolveError" class="notice danger" style="margin-top: 12px">{{ resolveError }}</div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">{{ t('仓库信息与文件选择') }}</div>
            <div class="card-subtitle">{{ t('{0}', [resolved ? `${resolved.repoId}@${resolved.revision}` : t('解析后显示文件列表')]) }}</div>
          </div>
          <div class="card-header-actions">
            <span v-if="resolved" class="badge no-dot info">{{ t('{0} 文件', [resolved.fileCount]) }}</span>
            <span v-if="resolved" class="badge no-dot muted">{{ humanBytes(resolved.totalSize) }}</span>
          </div>
        </div>
        <div class="card-body" :class="{ flush: !resolved }">
          <div v-if="!resolved" class="empty-state" style="padding: 42px 16px">
            <div class="empty-icon"><Icon name="downloads" :size="22" /></div>
            <div class="empty-title">{{ t('尚未解析仓库') }}</div>
            <div class="empty-copy">{{ t('输入 Hugging Face 仓库地址后点击“解析仓库”，会读取官方 API 的真实文件树、大小与 LFS SHA256。') }}</div>
          </div>
          <template v-else>
            <div class="repo-head">
              <div>
                <div class="repo-name">{{ resolved.repoId }}</div>
                <div class="repo-meta">
                  <span>{{ t('作者 {0}', [resolved.author || '—']) }}</span>
                  <span class="mono">commit {{ (resolved.commitHash || '').slice(0, 12) }}</span>
                  <span v-if="resolved.license">{{ t('许可 {0}', [resolved.license]) }}</span>
                  <span v-if="resolved.library">{{ resolved.library }}</span>
                  <span v-if="resolved.private" class="badge danger no-dot">{{ t('私有仓库') }}</span>
                  <span v-if="resolved.gated" class="badge warning no-dot">{{ t('需接受协议') }}</span>
                </div>
              </div>
              <div class="repo-actions">
                <button class="btn btn-sm" @click="filesExpanded = !filesExpanded"> {{ t('{0}', [filesExpanded ? t('收起文件') : t('展开文件')]) }}</button>
              </div>
            </div>

            <div v-if="filesExpanded">
              <div class="toolbar">
                <input v-model="fileSearch" class="input mono" style="max-width: 240px" :placeholder="t('搜索文件名')" />
                <select v-model="categoryFilter" class="input" style="max-width: 150px">
                  <option v-for="category in categories" :key="category" :value="category"> {{ t('{0}', [category === 'all' ? t('全部类型') : (categoryLabels[category] || category)]) }}</option>
                </select>
                <input v-model="includePattern" class="input mono" style="max-width: 180px" :placeholder="t('包含 *.safetensors')" />
                <input v-model="excludePattern" class="input mono" style="max-width: 160px" :placeholder="t('排除 *.onnx')" />
                <div class="toolbar-spacer"></div>
                <button class="btn btn-sm" @click="selectVisible">{{ t('选择可见') }}</button>
                <button class="btn btn-sm" @click="invertVisible">{{ t('反选') }}</button>
                <button class="btn btn-sm" @click="selectWeights">{{ t('仅权重') }}</button>
                <button class="btn btn-sm" @click="clearSelection">{{ t('清空') }}</button>
              </div>

              <div class="file-list">
                <table class="table">
                  <thead>
                    <tr>
                      <th style="width: 36px"></th>
                      <th>{{ t('文件路径') }}</th>
                      <th style="width: 110px">{{ t('类型') }}</th>
                      <th style="width: 110px" class="num">{{ t('大小') }}</th>
                      <th style="width: 160px">SHA256</th>
                    </tr>
                  </thead>
                  <tbody>
                    <tr v-for="file in visibleFiles" :key="file.path" @click="toggleFile(file.path)">
                      <td><input type="checkbox" :checked="selectedPaths.includes(file.path)" @click.stop="toggleFile(file.path)" /></td>
                      <td class="mono strong">{{ file.path }}</td>
                      <td><span class="badge no-dot" :class="categoryBadge(file.category)">{{ categoryLabels[file.category] || file.category }}</span></td>
                      <td class="mono num">{{ humanBytes(file.size) }}</td>
                      <td class="mono small muted">{{ file.sha256 ? file.sha256.slice(0, 14) + '…' : '—' }}</td>
                    </tr>
                    <tr v-if="!visibleFiles.length">
                      <td colspan="5" class="muted" style="text-align: center; padding: 18px">{{ t('没有符合筛选条件的文件') }}</td>
                    </tr>
                  </tbody>
                </table>
              </div>

              <div class="selection-bar">
                <span>{{ t('已选') }} <strong>{{ selectedPaths.length }}</strong> {{ t('/ {0} 个文件', [resolved.fileCount]) }}</span>
                <span class="mono">{{ humanBytes(selectedBytes) }}</span>
                <div class="toolbar-spacer"></div>
                <button class="btn btn-primary" :disabled="!selectedPaths.length || busy === 'create'" @click="startDownload">
                  <Icon name="download" :size="13" /> {{ t('{0}', [busy === 'create' ? t('创建中…') : t('开始下载')]) }}</button>
              </div>
            </div>
          </template>
        </div>
      </div>
    </div>

    <div class="card" style="margin-top: 16px">
      <div class="card-header">
        <div>
          <div class="card-title">{{ t('下载任务') }}</div>
          <div class="card-subtitle">{{ t('进度来自实际写入字节；暂停、继续与取消会持久化到 SQLite') }}</div>
        </div>
        <div class="card-header-actions">
          <span class="badge no-dot" :class="activeTasks.length ? 'active' : 'muted'">{{ t('{0} 个进行中', [activeTasks.length]) }}</span>
        </div>
      </div>
      <div class="card-body flush">
        <div class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th>{{ t('任务') }}</th>
                <th style="width: 210px">{{ t('进度') }}</th>
                <th style="width: 150px">{{ t('速度 / 剩余') }}</th>
                <th style="width: 120px">{{ t('文件') }}</th>
                <th style="width: 240px">{{ t('操作') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="task in tasks" :key="task.id" :class="{ selected: selectedTaskId === task.id }" @click="selectTask(task.id)">
                <td>
                  <div class="strong">{{ task.repoId }}<span class="muted mono"> @{{ task.revision }}</span>
                    <span v-if="task.repoType === 'dataset'" class="badge no-dot warning" style="margin-left: 6px">{{ t('数据集') }}</span>
                  </div>
                  <div class="small muted mono">{{ task.id }} · {{ formatTime(task.createdAt) }}</div>
                  <div v-if="task.currentFiles" class="small muted ellipsis">{{ t('正在下载 {0}', [task.currentFiles]) }}</div>
                  <div v-if="task.errorMessage" class="small" style="color: var(--danger)">{{ task.errorMessage }}</div>
                </td>
                <td>
                  <ProgressBar :value="task.progress" :label="t(task.statusLabel)" :meta="`${humanBytes(task.downloadedBytes)} / ${humanBytes(task.totalBytes)}`" :danger="task.status === 'failed'" />
                </td>
                <td class="mono small">
                  <div>{{ task.speed > 0 ? humanBytes(task.speed) + '/s' : '—' }}</div>
                  <div class="muted">{{ task.speed > 0 ? formatEta(task) : '—' }}</div>
                </td>
                <td class="mono small">{{ task.completedFiles }} / {{ task.fileCount }}</td>
                <td @click.stop>
                  <div class="table-actions">
                    <button v-if="task.status === 'downloading' || task.status === 'retrying' || task.status === 'verifying'" class="btn btn-sm" :disabled="busy === 'pause'" @click="taskAction(task, 'pause')"><Icon name="pause" :size="12" /> {{ t('暂停') }}</button>
                    <button v-if="task.status === 'paused' || task.status === 'pending'" class="btn btn-sm" :disabled="busy === 'resume'" @click="taskAction(task, 'resume')"><Icon name="resume" :size="12" /> {{ t('继续') }}</button>
                    <button v-if="['downloading','retrying','verifying','paused','pending'].includes(task.status)" class="btn btn-sm btn-danger" :disabled="busy === 'cancel'" @click="taskAction(task, 'cancel')"><Icon name="stop" :size="12" /> {{ t('取消') }}</button>
                    <button v-if="task.status === 'failed' || task.status === 'cancelled'" class="btn btn-sm" :disabled="busy === 'retry'" @click="taskAction(task, 'retry')"><Icon name="retry" :size="12" /> {{ t('重试') }}</button>
                    <button class="btn btn-sm btn-ghost" :disabled="busy === 'delete'" @click="taskAction(task, 'delete')"><Icon name="trash" :size="12" /></button>
                  </div>
                </td>
              </tr>
              <tr v-if="!tasks.length">
                <td colspan="5">
                  <div class="empty-state" style="padding: 28px">
                    <div class="empty-title">{{ t('还没有下载任务') }}</div>
                    <div class="empty-copy">{{ t('解析仓库并选择文件后创建任务；任务记录会持久化，重启后可恢复。') }}</div>
                  </div>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </div>

    <div v-if="detailTask" class="card" style="margin-top: 16px">
      <div class="card-header">
        <div>
          <div class="card-title">{{ t('任务详情 · {0}', [detailTask.id]) }}</div>
          <div class="card-subtitle mono">{{ detailTask.repoId }}@{{ detailTask.revision }} → {{ detailTask.destination }}</div>
        </div>
        <div class="card-header-actions">
          <span class="badge" :class="taskTone(detailTask.status)">{{ t(detailTask.statusLabel) }}</span>
          <button class="btn btn-sm" @click="copyPath(detailTask.destination)"><Icon name="copy" :size="12" /> {{ t('复制路径') }}</button>
        </div>
      </div>
      <div class="card-body">
        <div class="kv-grid">
          <div class="kv"><span>{{ t('总大小') }}</span><span class="mono">{{ humanBytes(detailTask.totalBytes) }}</span></div>
          <div class="kv"><span>{{ t('已下载') }}</span><span class="mono">{{ humanBytes(detailTask.downloadedBytes) }}</span></div>
          <div class="kv"><span>{{ t('速度') }}</span><span class="mono">{{ detailTask.speed > 0 ? humanBytes(detailTask.speed) + '/s' : '—' }}</span></div>
          <div class="kv"><span>{{ t('剩余时间') }}</span><span class="mono">{{ formatEta(detailTask) }}</span></div>
          <div class="kv"><span>{{ t('并发') }}</span><span class="mono">{{ detailTask.concurrency }}</span></div>
          <div class="kv"><span>{{ t('完成文件') }}</span><span class="mono">{{ detailTask.completedFiles }} / {{ detailTask.fileCount }}</span></div>
          <div class="kv"><span>Commit</span><span class="mono">{{ (detailTask.commitHash || '').slice(0, 12) || '—' }}</span></div>
          <div class="kv"><span>{{ t('完成时间') }}</span><span class="mono">{{ detailTask.completedAt ? formatTime(detailTask.completedAt) : '—' }}</span></div>
        </div>

        <div v-if="detailTask.result?.summary" class="notice" :class="detailTask.result.kind === 'gguf' ? 'info' : 'success'" style="margin: 12px 0">
          <Icon name="info" :size="14" class="notice-icon" />
          <div>
            {{ detailTask.result.summary }}
            <div v-if="detailTask.result.modelId" style="margin-top: 8px">
              <router-link class="btn btn-sm btn-primary" :to="`/convert?model=${detailTask.result.modelId}`">{{ t('进入转换') }} <Icon name="arrow" :size="12" /></router-link>
              <router-link class="btn btn-sm" :to="`/models`" style="margin-left: 8px">{{ t('查看模型库') }}</router-link>
            </div>
            <div v-if="detailTask.result.ggufPath" class="mono small" style="margin-top: 6px">
              {{ detailTask.result.ggufPath }}
              <router-link class="btn btn-sm" :to="`/gguf?path=${encodeURIComponent(detailTask.result.ggufPath)}`" style="margin-left: 8px">{{ t('GGUF 检查器') }}</router-link>
            </div>
          </div>
        </div>

        <div class="section-label">{{ t('文件级状态') }}</div>
        <div class="table-wrap">
          <table class="table">
            <thead>
              <tr>
                <th>{{ t('远程路径') }}</th>
                <th style="width: 110px">{{ t('状态') }}</th>
                <th style="width: 190px">{{ t('进度') }}</th>
                <th style="width: 100px">{{ t('重试') }}</th>
                <th>{{ t('说明') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="file in detailFiles" :key="file.id">
                <td class="mono small">{{ file.remotePath }}</td>
                <td><span class="badge no-dot" :class="taskTone(file.status === 'completed' ? 'completed' : file.status)">{{ file.status }}</span></td>
                <td>
                  <ProgressBar :value="file.size ? Math.min(100, Math.round((file.downloaded / file.size) * 100)) : 0" :meta="`${humanBytes(file.downloaded)} / ${humanBytes(file.size)}`" />
                </td>
                <td class="mono small">{{ file.retryCount }}</td>
                <td class="small muted">{{ t('{0}', [file.error || (file.checksum ? t('SHA256 已提供') : t('无远端哈希'))]) }}</td>
              </tr>
            </tbody>
          </table>
        </div>

        <div class="section-label" style="margin-top: 16px">{{ t('下载日志') }}</div>
        <div class="log-box">
          <div v-for="entry in detailLogs" :key="entry.seq" class="log-line">
            <span class="log-time mono">{{ entry.time }}</span>
            <span class="log-level" :class="entry.level.toLowerCase()">{{ entry.level }}</span>
            <span class="log-message">{{ entry.message }}</span>
          </div>
          <div v-if="!detailLogs.length" class="muted small" style="padding: 12px">{{ t('暂无日志') }}</div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.dl-grid {
  display: grid;
  grid-template-columns: minmax(340px, 0.85fr) minmax(0, 1.6fr);
  gap: 16px;
  align-items: start;
}
@media (max-width: 1180px) {
  .dl-grid { grid-template-columns: 1fr; }
}
.check-line {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: var(--muted);
  margin-top: 6px;
  cursor: pointer;
}
.token-row {
  display: flex;
  gap: 8px;
  align-items: center;
}
.token-row .input { flex: 1; }
.inline-ok {
  margin-top: 8px;
  font-size: 12px;
  color: var(--success, #3fb950);
  display: flex;
  align-items: center;
  gap: 6px;
}
.repo-head {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--border);
  margin-bottom: 12px;
}
.repo-name { font-size: 15px; font-weight: 600; }
.repo-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-top: 6px;
  font-size: 11.5px;
  color: var(--muted);
}
.file-list {
  max-height: 420px;
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: 8px;
  margin-top: 10px;
}
.file-list .table thead th {
  position: sticky;
  top: 0;
  background: var(--panel, #161b22);
  z-index: 1;
}
.selection-bar {
  display: flex;
  align-items: center;
  gap: 14px;
  margin-top: 12px;
  font-size: 12px;
  color: var(--muted);
}
.table tr.selected td { background: rgba(56, 139, 253, 0.08); }
.ellipsis { max-width: 420px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.log-box {
  max-height: 280px;
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--code-bg, #0d1117);
  font-size: 11.5px;
  padding: 6px 0;
}
.log-line {
  display: grid;
  grid-template-columns: 150px 70px minmax(0, 1fr);
  gap: 8px;
  padding: 3px 12px;
  font-family: var(--font-mono);
}
.log-time { color: var(--faint); }
.log-level { font-weight: 600; }
.log-level.info { color: var(--info, #58a6ff); }
.log-level.warning { color: var(--warning, #d29922); }
.log-level.error { color: var(--danger, #f85149); }
.log-message { color: var(--text); white-space: pre-wrap; word-break: break-all; }
</style>
