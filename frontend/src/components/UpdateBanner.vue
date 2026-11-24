<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, humanBytes } from '../api/client'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'

interface StartupItem {
  component: 'application' | 'llama'
  name: string
  currentVersion: string
  latestVersion: string
  channel?: string
  prerelease?: boolean
  releaseName?: string
  publishedAt?: string
  htmlUrl?: string
  tag?: string
  assetName?: string
  assetSize?: number
  message?: string
}

const app = useAppStore()

const items = ref<StartupItem[]>([])
const dismissed = ref(false)
// Avoid hitting the GitHub API on every SPA reload; the server caches llama.cpp
// checks for 10 minutes anyway.
const CACHE_KEY = 'modelforge.startupUpdate.v1'
const HIDDEN_KEY = 'modelforge.startupUpdateHidden.v1'
const CACHE_TTL_MS = 10 * 60 * 1000
const busy = ref('')
const progress = ref(0)
const progressText = ref('')

const visible = computed(() => !dismissed.value && items.value.length > 0)

const channelLabels: Record<string, string> = {
  stable: t('稳定版'),
  nightly: t('每日构建'),
  auto: t('自动')
}

function componentLabel(component: string): string {
  return component === 'llama' ? 'llama.cpp' : 'ModelForge'
}

function tone(component: string): string {
  return component === 'llama' ? 'info' : 'warning'
}

function signature(list: StartupItem[]): string {
  return list.map((item) => `${item.component}@${item.latestVersion}`).join(',')
}

function readCache(): { at: number; items: StartupItem[] } | null {
  try {
    const raw = sessionStorage.getItem(CACHE_KEY)
    if (!raw) return null
    const parsed = JSON.parse(raw)
    if (!parsed || typeof parsed.at !== 'number' || !Array.isArray(parsed.items)) return null
    if (Date.now() - parsed.at > CACHE_TTL_MS) return null
    return parsed
  } catch {
    return null
  }
}

function writeCache(list: StartupItem[]) {
  try {
    sessionStorage.setItem(CACHE_KEY, JSON.stringify({ at: Date.now(), items: list }))
  } catch {
    /* private mode: caching is optional */
  }
}

function isHidden(list: StartupItem[]): boolean {
  try {
    return sessionStorage.getItem(HIDDEN_KEY) === signature(list)
  } catch {
    return false
  }
}

function dismiss() {
  dismissed.value = true
  try {
    sessionStorage.setItem(HIDDEN_KEY, signature(items.value))
  } catch {
    /* ignore */
  }
}

function remove(component: string) {
  items.value = items.value.filter((item) => item.component !== component)
  if (!items.value.length) dismissed.value = true
}

async function startUpdate(item: StartupItem) {
  if (busy.value) return
  busy.value = item.component
  progress.value = 0
  progressText.value = t('正在准备更新…')
  try {
    if (item.component === 'application') await updateApplication()
    else await updateLlama(item)
  } catch (error) {
    app.fromError(error, t('更新失败'))
  } finally {
    busy.value = ''
    progress.value = 0
    progressText.value = ''
  }
}

async function updateApplication() {
  const started = await api.post<{ jobId: string; message: string }>('/api/v1/updates/download')
  progressText.value = started.message || t('正在下载更新…')
  const deadline = Date.now() + 10 * 60 * 1000
  while (Date.now() < deadline) {
    await new Promise((resolve) => window.setTimeout(resolve, 1200))
    const job = await api.get<any>(`/api/v1/updates/job/${encodeURIComponent(started.jobId)}`)
    progress.value = Number(job.progress || 0)
    if (job.message) progressText.value = job.message
    if (job.status === 'ready') {
      await api.post('/api/v1/updates/apply', { jobId: started.jobId })
      app.toast('success', t('更新已下载'), t('程序退出后会自动替换文件并重启。'), '', 12000)
      remove('application')
      return
    }
    if (job.status === 'failed') {
      throw new Error(job.message || t('更新下载失败'))
    }
  }
  throw new Error(t('更新下载超时，请到设置页重试'))
}

async function updateLlama(item: StartupItem) {
  if (!item.tag || !item.assetName) throw new Error(t('缺少可安装的官方资源'))
  const started = await api.post<any>('/api/v1/llama/install', { tag: item.tag, asset: item.assetName })
  progress.value = Number(started.progress || 0)
  progressText.value = started.message || t('正在下载官方资源…')
  const deadline = Date.now() + 10 * 60 * 1000
  while (Date.now() < deadline) {
    await new Promise((resolve) => window.setTimeout(resolve, 1200))
    const data = await api.get<any>('/api/v1/llama/install/status')
    const job = data.job || {}
    progress.value = Number(job.progress || 0)
    if (job.message) progressText.value = job.message
    if (job.status === 'completed') {
      app.toast('success', t('llama.cpp 已更新'), job.message || item.latestVersion)
      await app.refreshCapabilities(true)
      remove('llama')
      return
    }
    if (job.status === 'failed') {
      throw new Error(job.errorMessage || job.message || t('安装失败'))
    }
    if (job.status === 'cancelled') throw new Error(t('安装已取消'))
  }
  throw new Error(t('更新下载超时，请到运行引擎页重试'))
}

onMounted(async () => {
  await app.init()
  if (app.settings?.autoCheckUpdates === false) return
  const cached = readCache()
  if (cached) {
    items.value = cached.items.filter((item) => item.latestVersion)
    dismissed.value = items.value.length > 0 && isHidden(items.value)
    return
  }
  try {
    const data = await api.get<{ enabled: boolean; items: StartupItem[] }>('/api/v1/updates/startup')
    if (!data?.enabled) return
    const found = (data.items || []).filter((item) => item.latestVersion)
    items.value = found
    if (found.length) {
      writeCache(found)
      dismissed.value = isHidden(found)
    }
  } catch {
    // Offline / rate-limited / server error: stay silent and never block startup.
    items.value = []
  }
})
</script>

<template>
  <div v-if="visible" class="notice update-banner" :class="items.length === 1 ? tone(items[0].component) : 'info'" role="status">
    <Icon name="download" :size="15" class="notice-icon" />
    <div class="update-body">
      <strong>{{ t('发现可用更新') }}</strong>
      <div v-for="item in items" :key="item.component" class="update-row">
        <div class="update-meta">
          <span class="update-name">{{ componentLabel(item.component) }}</span>
          <span class="update-version mono">{{ item.currentVersion || t('未安装') }} → {{ item.latestVersion }}</span>
          <span v-if="item.channel" class="badge no-dot muted">{{ channelLabels[item.channel] || item.channel }}</span>
          <span v-if="item.prerelease" class="badge no-dot warning">{{ t('预发布') }}</span>
          <span v-if="item.assetSize" class="small muted">{{ humanBytes(item.assetSize) }}</span>
        </div>
        <button class="btn btn-sm btn-primary" :disabled="Boolean(busy)" @click="startUpdate(item)">
          <span v-if="busy === item.component" class="spinner"></span>
          <Icon v-else name="download" :size="12" />
          {{ t('{0}', [busy === item.component ? t('更新中…') : t('更新')]) }}
        </button>
      </div>
      <div v-if="busy && progressText" class="update-progress">
        <span class="small muted">{{ progressText }}</span>
        <div class="update-bar"><span :style="{ width: `${Math.max(3, progress)}%` }"></span></div>
      </div>
      <a v-if="items.length === 1 && items[0].htmlUrl" class="update-link" :href="items[0].htmlUrl" target="_blank" rel="noopener">
        {{ t('查看发布说明') }}
      </a>
    </div>
    <button class="icon-btn update-close" :disabled="Boolean(busy)" :title="t('本次不再提示')" @click="dismiss">
      <Icon name="close" :size="12" />
    </button>
  </div>
</template>

<style scoped>
.update-banner {
  align-items: flex-start;
}
.update-body {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.update-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
}
.update-meta {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  min-width: 0;
}
.update-name {
  font-weight: 600;
}
.update-version {
  font-size: 12px;
}
.update-progress {
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-width: 520px;
}
.update-bar {
  height: 4px;
  border-radius: 999px;
  background: var(--border, rgba(127, 127, 127, 0.25));
  overflow: hidden;
}
.update-bar span {
  display: block;
  height: 100%;
  background: var(--accent, #4f8cff);
  transition: width 0.3s ease;
}
.update-link {
  font-size: 11px;
  color: var(--muted, #8a93a6);
}
.update-close {
  flex: 0 0 auto;
}
</style>
