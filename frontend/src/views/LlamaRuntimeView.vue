<script setup lang="ts">
import { t } from '../i18n'
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { api, humanBytes, formatTime } from '../api/client'
import type { Settings } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import ProgressBar from '../components/ProgressBar.vue'

const app = useAppStore()

const runtime = ref<any>(null)
const probe = ref<any>(null)
const job = ref<any>(null)
const check = ref<any>(null)
const assets = ref<any[]>([])
const statusText = ref('idle')
const loading = ref(true)
const checking = ref(false)
const installing = ref(false)
const saving = ref(false)
const selectedAsset = ref('')
const form = ref<Pick<Settings, 'llamaRoot' | 'llamaBackend' | 'llamaChannel'>>({
  llamaRoot: '',
  llamaBackend: 'auto',
  llamaChannel: 'auto'
})
let pollTimer: number | null = null

const backendOptions = [
  { value: 'auto', label: t('自动（按硬件选择）') },
  { value: 'cpu', label: 'CPU' },
  { value: 'cuda', label: 'CUDA' },
  { value: 'vulkan', label: 'Vulkan' },
  { value: 'rocm', label: 'ROCm / HIP' },
  { value: 'sycl', label: 'SYCL' },
  { value: 'openvino', label: 'OpenVINO' },
  { value: 'opencl', label: 'OpenCL' }
]

const statusTone = computed(() => {
  const status = check.value?.status
  if (status === 'up_to_date' || status === 'local_newer') return 'success'
  if (status === 'update_available' || status === 'local_version_unknown') return 'warning'
  if (status === 'not_installed' || status === 'asset_mismatch') return 'danger'
  return 'muted'
})

const statusLabel = computed(() => {
  const status = check.value?.status
  const map: Record<string, string> = {
    up_to_date: t('已是最新'),
    update_available: t('有可用更新'),
    local_newer: t('本地版本更新'),
    not_installed: t('未安装'),
    local_version_unknown: t('本地版本未知'),
    asset_mismatch: t('无匹配资源')
  }
  return map[status] || t('未检查')
})

const jobActive = computed(() => {
  const status = job.value?.status
  return status === 'downloading' || status === 'verifying' || status === 'extracting'
})

async function load() {
  loading.value = true
  try {
    const [status, settings] = await Promise.all([
      api.get<any>('/api/v1/llama/status'),
      api.get<Settings>('/api/v1/settings')
    ])
    runtime.value = status.runtime
    probe.value = status.probe
    job.value = status.job
    form.value = {
      llamaRoot: settings.llamaRoot || '',
      llamaBackend: settings.llamaBackend || 'auto',
      llamaChannel: settings.llamaChannel || 'auto'
    }
    if (jobActive.value) startPolling()
  } catch (error) {
    app.fromError(error, t('加载运行时状态失败'))
  } finally {
    loading.value = false
  }
}

async function saveSettings() {
  saving.value = true
  try {
    const result = await api.put<{ settings: Settings }>('/api/v1/settings', form.value)
    app.settings = result.settings
    app.toast('success', t('运行时配置已保存'), t('工具路径将由后台重新检测'))
    await load()
    if (check.value) await runCheck(true)
  } catch (error) {
    app.fromError(error, t('保存配置失败'))
  } finally {
    saving.value = false
  }
}

async function runCheck(force = true) {
  if (checking.value) return
  checking.value = true
  statusText.value = 'checking'
  try {
    const data = await api.post<any>('/api/v1/llama/update/check', { force })
    check.value = data
    assets.value = data.available || []
    selectedAsset.value = data.asset?.name || ''
    if (data.status === 'update_available') {
      app.toast('info', t('发现新版本 ') + data.remoteTag, data.asset?.name || '', data.fallbackReason || '')
    } else if (data.status === 'up_to_date') {
      app.toast('success', t('已是最新官方版本'), data.remoteTag)
    } else if (data.status === 'asset_mismatch') {
      app.toast('warning', t('没有匹配当前平台的官方资源'), data.message || '')
    }
    statusText.value = 'idle'
  } catch (error: any) {
    statusText.value = 'error'
    // Network / rate limit / not-found are legitimate answers, not crashes.
    check.value = {
      status: error?.code === 'NO_NETWORK' ? 'no_network' : 'check_failed',
      message: error?.message || t('检查更新失败'),
      suggestion: error?.suggestion || ''
    }
    app.fromError(error, t('检查更新失败'))
  } finally {
    checking.value = false
  }
}

async function loadAssets(tag: string) {
  if (!tag) return
  try {
    const data = await api.get<any>(`/api/v1/llama/releases/${encodeURIComponent(tag)}/assets`)
    assets.value = (data.assets || []).filter((item: any) => !item.dependency)
  } catch (error) {
    app.fromError(error, t('获取版本资源失败'))
  }
}

async function install(assetName?: string) {
  const name = assetName || selectedAsset.value || check.value?.asset?.name
  if (!name || installing.value) return
  installing.value = true
  try {
    const started = await api.post<any>('/api/v1/llama/install', {
      tag: check.value?.remoteTag || check.value?.release?.tag || '',
      asset: name
    })
    job.value = started
    startPolling()
    app.toast('info', t('开始下载官方资源'), name)
  } catch (error) {
    app.fromError(error, t('无法开始安装'))
  } finally {
    installing.value = false
  }
}

async function cancelInstall() {
  try {
    job.value = await api.post<any>('/api/v1/llama/install/cancel')
    app.toast('info', t('已请求取消'), t('已下载的分片会保留以便续传'))
  } catch (error) {
    app.fromError(error, t('取消失败'))
  }
}

function startPolling() {
  if (pollTimer !== null) return
  pollTimer = window.setInterval(async () => {
    try {
      const data = await api.get<any>('/api/v1/llama/install/status')
      job.value = data.job
      runtime.value = data.runtime
      if (!jobActive.value && pollTimer !== null) {
        window.clearInterval(pollTimer)
        pollTimer = null
        if (job.value?.status === 'completed') {
          app.toast('success', t('llama.cpp 已更新'), job.value.message)
          await runCheck(true)
          await app.refreshCapabilities(true)
        } else if (job.value?.status === 'failed') {
          app.toast('error', t('安装失败'), job.value.errorMessage, job.value.errorSuggestion, 9000)
        }
      }
    } catch {
      if (pollTimer !== null) {
        window.clearInterval(pollTimer)
        pollTimer = null
      }
    }
  }, 1000)
}

onMounted(async () => {
  await app.init()
  await load()
})

onBeforeUnmount(() => {
  if (pollTimer !== null) window.clearInterval(pollTimer)
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('运行引擎') }}</h1>
        <p class="page-subtitle">
          {{ t('随包分发的 llama.cpp 是官方 release 原样解压产物；此处识别本地官方版本、检查更新，并按当前平台下载官方资源、校验 sha256 后原样解压。') }}
        </p>
      </div>
      <div class="page-actions">
        <button class="btn" :disabled="loading" @click="load"><Icon name="refresh" :size="13" /> {{ t('重新检测') }}</button>
        <button class="btn btn-primary" :disabled="checking" @click="runCheck(true)">
          <span v-if="checking" class="spinner"></span><Icon v-else name="activity" :size="13" /> {{ t('检查更新') }}
        </button>
      </div>
    </div>

    <div v-if="loading || !runtime" class="loading-panel"><span class="spinner"></span> {{ t('正在检测本地 llama.cpp…') }}</div>

    <template v-else>
      <div v-if="check" class="notice" :class="statusTone === 'success' ? 'success' : statusTone === 'danger' ? 'warning' : 'warning'" style="margin-bottom: 12px">
        <Icon :name="statusTone === 'success' ? 'check' : statusTone === 'danger' ? 'warning' : 'info'" :size="14" class="notice-icon" />
        <div>
          <strong>{{ statusLabel }}：</strong>{{ check.message || '—' }}
          <div v-if="check.suggestion" class="small muted" style="margin-top: 2px">{{ check.suggestion }}</div>
          <div v-if="check.fallbackReason" class="small muted" style="margin-top: 2px">{{ check.fallbackReason }}</div>
          <div v-if="check.backendHint" class="small muted" style="margin-top: 2px">{{ check.backendHint.label }}</div>
        </div>
      </div>

      <div class="split-panel">
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">{{ t('本地运行时') }}</div>
              <div class="card-subtitle">{{ t('版本号与路径均来自磁盘上的官方发行包') }}</div>
            </div>
            <span class="badge" :class="runtime.installed ? 'success' : 'danger'"> {{ t('{0}', [runtime.installed ? runtime.versionLabel : t('未安装')]) }}</span>
          </div>
          <div class="card-body">
            <div class="kv"><span>{{ t('安装目录') }}</span><span class="mono">{{ runtime.root }}</span></div>
            <div class="kv"><span>{{ t('发行目录') }}</span><span class="mono">{{ runtime.releaseDir || '—' }}</span></div>
            <div class="kv"><span>{{ t('加速后端') }}</span><span>{{ runtime.backendLabel }}</span></div>
            <div class="kv"><span>{{ t('识别来源') }}</span><span>{{ runtime.detectionSource }}</span></div>
            <div class="kv">
              <span>{{ t('完整性') }}</span>
              <span>
                <template v-if="runtime.integrity?.sha256"> {{ t('sha256 已校验 · {0}', [humanBytes(runtime.integrity.size)]) }}</template>
                <template v-else>{{ t('未记录校验值（手动安装）') }}</template>
              </span>
            </div>
            <div class="kv"><span>{{ t('资源包') }}</span><span class="mono">{{ runtime.asset || '—' }}</span></div>

            <div class="section-label">{{ t('工具链') }}</div>
            <div class="check-item" v-for="tool in [
              { key: 'quantize', label: 'llama-quantize', path: runtime.tools?.quantize, ok: runtime.tools?.quantizeAvailable },
              { key: 'cli', label: 'llama-cli', path: runtime.tools?.cli, ok: runtime.tools?.cliAvailable },
              { key: 'server', label: 'llama-server', path: runtime.tools?.server, ok: runtime.tools?.serverAvailable },
              { key: 'bench', label: 'llama-bench', path: runtime.tools?.bench, ok: runtime.tools?.benchAvailable }
            ]" :key="tool.key">
              <span class="check-mark" :class="tool.ok ? 'pass' : 'fail'">
                <Icon :name="tool.ok ? 'check' : 'close'" :size="9" :stroke="2" />
              </span>
              <div style="min-width: 0">
                <div class="check-title">{{ tool.label }}</div>
                <div class="check-detail mono">{{ t('{0}', [tool.path || t('未找到')]) }}</div>
              </div>
            </div>
            <div class="check-item">
              <span class="check-mark" :class="runtime.convertScriptAvailable ? 'pass' : 'fail'">
                <Icon :name="runtime.convertScriptAvailable ? 'check' : 'close'" :size="9" :stroke="2" />
              </span>
              <div style="min-width: 0">
                <div class="check-title">convert_hf_to_gguf.py</div>
                <div class="check-detail mono">{{ t('{0}', [runtime.convertScript || t('未找到')]) }}</div>
              </div>
            </div>
            <div v-for="issue in runtime.issues || []" :key="issue" class="notice warning" style="margin-top: 8px">
              <Icon name="warning" :size="13" class="notice-icon" /><div>{{ issue }}</div>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">{{ t('平台与更新') }}</div>
              <div class="card-subtitle">{{ t('资源选择由硬件探测与设置共同决定') }}</div>
            </div>
            <span class="badge" :class="statusTone">{{ statusLabel }}</span>
          </div>
          <div class="card-body">
            <div class="kv"><span>{{ t('平台') }}</span><span>{{ probe?.platform }} · {{ probe?.arch }}</span></div>
            <div class="kv"><span>{{ t('NVIDIA 驱动') }}</span><span>{{ t('{0}', [probe?.nvidia ? (probe.nvidiaDriver || t('已检测')) + (probe.cudaVersion ? ' · CUDA ' + probe.cudaVersion : '') : t('未检测到')]) }}</span></div>
            <div class="kv"><span>Vulkan</span><span>{{ t('{0}', [probe?.vulkan ? t('可用') : t('不可用')]) }}</span></div>
            <div class="kv"><span>{{ t('后端选择链') }}</span><span>{{ (probe?.chain || []).join(' → ') }}</span></div>
            <template v-if="check">
              <div class="kv"><span>{{ t('官方最新') }}</span><span class="mono">{{ check.remoteTag || check.release?.tag || '—' }}</span></div>
              <div class="kv"><span>{{ t('发布时间') }}</span><span>{{ check.release?.publishedAt ? formatTime(check.release.publishedAt) : '—' }}</span></div>
              <div class="kv"><span>{{ t('匹配资源') }}</span><span class="mono">{{ check.asset?.name || '—' }}</span></div>
              <div class="kv"><span>{{ t('资源大小') }}</span><span>{{ check.asset?.sizeLabel || '—' }}</span></div>
              <div class="kv"><span>{{ t('校验方式') }}</span><span>{{ t('{0}', [check.asset?.digest ? t('GitHub sha256 摘要') : t('仅文件大小（官方未提供摘要）')]) }}</span></div>
            </template>

            <div class="section-label">{{ t('下载与安装') }}</div>
            <div v-if="assets.length" class="field">
              <label class="field-label">{{ t('官方资源') }}</label>
              <select v-model="selectedAsset" class="select">
                <option v-for="asset in assets" :key="asset.name" :value="asset.name">
                  {{ asset.name }} · {{ asset.sizeLabel }} · {{ asset.backendLabel }}
                </option>
              </select>
              <div class="field-hint">{{ t('列表来自同一版本的官方 release 资产，未经过任何重命名或重新打包。') }}</div>
            </div>
            <div class="field-row" style="margin-top: 8px">
              <button class="btn btn-primary" :disabled="installing || jobActive || !selectedAsset" @click="install()">
                <span v-if="installing" class="spinner"></span><Icon v-else name="download" :size="13" /> {{ t('{0}', [runtime.installed ? t('下载并更新') : t('下载并安装')]) }}</button>
              <button v-if="jobActive" class="btn" @click="cancelInstall"><Icon name="stop" :size="13" /> {{ t('取消') }}</button>
              <button v-if="check?.release?.tag" class="btn" @click="loadAssets(check.release.tag)">
                <Icon name="refresh" :size="13" /> {{ t('刷新资源列表') }}
              </button>
            </div>

            <div v-if="job && job.status !== 'idle'" style="margin-top: 12px">
              <ProgressBar
                :value="job.progress"
                :mode="job.status === 'extracting' || job.status === 'verifying' ? 'stage' : 'measured'"
                :label="job.statusLabel + ' · ' + (job.message || '')"
                :meta="job.downloadedLabel + ' / ' + job.totalLabel + (job.speed ? ' · ' + job.speedLabel : '')"
                :danger="job.status === 'failed'"
              />
              <div v-if="job.status === 'failed'" class="notice warning" style="margin-top: 8px">
                <Icon name="warning" :size="13" class="notice-icon" />
                <div>
                  <strong>[{{ job.errorCode }}]</strong> {{ job.errorMessage }}
                  <div v-if="job.errorSuggestion" class="small muted" style="margin-top: 2px">{{ t('建议：{0}', [job.errorSuggestion]) }}</div>
                </div>
              </div>
              <div v-if="job.artifacts?.length" class="small muted" style="margin-top: 6px">
                <div v-for="item in job.artifacts" :key="item.name">
                  {{ item.name }}<span v-if="item.dependency">{{ t('（CUDA 运行时依赖）') }}</span>
                  <span v-if="item.error"> {{ t('· 失败：{0}', [item.error]) }}</span>
                  <span v-else-if="item.directory"> · {{ item.directory }}</span>
                </div>
              </div>
            </div>

            <div class="section-label">{{ t('运行时配置') }}</div>
            <div class="field">
              <label class="field-label">{{ t('llama.cpp 目录（llamaRoot）') }}</label>
              <input v-model="form.llamaRoot" class="input mono" />
              <div class="field-hint">{{ t('官方 zip 会下载到该目录的 downloads/，并原样解压到 releases/<资产名>/。') }}</div>
            </div>
            <div class="field-row">
              <div class="field">
                <label class="field-label">{{ t('加速后端') }}</label>
                <select v-model="form.llamaBackend" class="select">
                  <option v-for="item in backendOptions" :key="item.value" :value="item.value">{{ item.label }}</option>
                </select>
              </div>
              <div class="field">
                <label class="field-label">{{ t('更新通道') }}</label>
                <select v-model="form.llamaChannel" class="select">
                  <option value="auto">{{ t('自动（最新可安装构建）') }}</option>
                  <option value="stable">{{ t('稳定版（优先正式 release）') }}</option>
                  <option value="nightly">{{ t('每日构建（含预发布）') }}</option>
                </select>
              </div>
            </div>
            <div class="field-row" style="margin-top: 8px">
              <button class="btn" :disabled="saving" @click="saveSettings">
                <span v-if="saving" class="spinner"></span><Icon v-else name="save" :size="13" /> {{ t('保存配置') }}
              </button>
              <div class="small muted" style="align-self: center">{{ t('保存后会自动重新检测并检查更新。') }}</div>
            </div>
          </div>
        </div>
      </div>

      <div class="card" style="margin-top: 14px">
        <div class="card-header">
          <div>
            <div class="card-title">{{ t('分发方式') }}</div>
            <div class="card-subtitle">{{ t('官方资产原样保留，不做裁剪、重命名或重新打包') }}</div>
          </div>
          <span class="tag">github.com/ggml-org/llama.cpp</span>
        </div>
        <div class="card-body">
          <div class="grid grid-2">
            <div class="notice success">
              <Icon name="check" :size="14" class="notice-icon" />
              <div><strong>{{ t('原样分发：') }}</strong>{{ t('发布包内的 llama.cpp 来自官方 release 资产，解压后保留全部文件、目录结构、版本号与二进制内容；ModelForge 自身只写') }} <span class="mono">modelforge-runtime.json</span> {{ t('记录来源与校验值。') }}</div>
            </div>
            <div class="notice warning">
              <Icon name="info" :size="14" class="notice-icon" />
              <div><strong>{{ t('兼容现有流程：') }}</strong>{{ t('下载完成后自动把 llama-quantize / llama-cli / llama-server / llama-bench 与转换脚本写回设置，转换、量化、测试与 CLI 调用无需改动。') }}</div>
            </div>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>
