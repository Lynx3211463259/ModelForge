<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, formatTime, relativeTime } from '../api/client'
import type { Settings } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import EmptyState from '../components/EmptyState.vue'
import DirectoryPicker from '../components/DirectoryPicker.vue'

const app = useAppStore()
const dirPickerOpen = ref(false)

function addWatchedFolder(path: string) {
  const existing = foldersText.value.split(/\r?\n/).map((item) => item.trim()).filter(Boolean)
  if (!existing.includes(path)) existing.push(path)
  foldersText.value = existing.join('\n')
}

const status = ref<any>(null)
const events = ref<any[]>([])
const profiles = ref<any[]>([])
const saving = ref(false)
const scanning = ref(false)
const scanResult = ref<any>(null)
const foldersText = ref('')
const interval = ref(30)
const autoConvert = ref(false)
const presetId = ref('')
let timer: number | undefined

const enabled = computed(() => Boolean(status.value?.enabled))
const folders = computed(() => foldersText.value.split('\n').map((line) => line.trim()).filter(Boolean))
const activePreset = computed(() => profiles.value.find((item) => item.id === presetId.value) || null)

async function load() {
  try {
    const [watchStatus, watchEvents, profileList] = await Promise.all([
      api.get<any>('/api/v1/watch/status'),
      api.get<any[]>('/api/v1/watch/events?limit=100'),
      api.get<any[]>('/api/v1/quantization/profiles')
    ])
    status.value = watchStatus
    events.value = watchEvents
    profiles.value = profileList
    if (!foldersText.value) foldersText.value = (watchStatus.folders || []).map((item: any) => item.path).join('\n')
    interval.value = watchStatus.intervalSec || 30
    autoConvert.value = Boolean(watchStatus.autoConvert)
    presetId.value = watchStatus.presetId || ''
  } catch (error) {
    app.fromError(error, t('读取监控状态失败'))
  }
}

async function save(patch: Record<string, any>, message: string) {
  saving.value = true
  try {
    await api.put('/api/v1/settings', patch)
    app.toast('success', message)
    await Promise.all([load(), app.refreshSettings?.()])
  } catch (error) {
    app.fromError(error, t('保存监控设置失败'))
  } finally {
    saving.value = false
  }
}

async function toggleEnabled() {
  await save({ folderWatchEnabled: !enabled.value }, enabled.value ? t('监控已暂停') : t('监控已启用'))
}

async function saveSettings() {
  await save(
    {
      watchFolders: folders.value,
      watchIntervalSec: Number(interval.value) || 30,
      watchAutoConvert: autoConvert.value,
      watchPresetId: presetId.value
    },
    t('监控设置已保存')
  )
}

async function scan(force: boolean) {
  if (!folders.value.length) {
    app.toast('warning', t('请先添加至少一个监控目录'))
    return
  }
  scanning.value = true
  scanResult.value = null
  try {
    scanResult.value = await api.post<any>('/api/v1/watch/scan', { force })
    app.toast('success', t('扫描完成'), t('导入 {0} · 跳过 {1} · 排队 {2} · 失败 {3}', [
      scanResult.value.imported, scanResult.value.skipped, scanResult.value.queued, scanResult.value.failed
    ]))
    await load()
  } catch (error) {
    app.fromError(error, t('扫描失败'))
  } finally {
    scanning.value = false
  }
}

function actionTone(action: string) {
  if (action === 'IMPORTED') return 'success'
  if (action === 'CONVERT_QUEUED') return 'info'
  if (action === 'FAILED' || action === 'CONVERT_FAILED') return 'danger'
  return 'muted'
}

function actionLabel(action: string) {
  const map: Record<string, string> = {
    IMPORTED: t('导入成功'),
    FAILED: t('导入失败'),
    CONVERT_QUEUED: t('已排队转换'),
    CONVERT_FAILED: t('转换入队失败'),
    CONVERT_SKIPPED: t('跳过转换')
  }
  return map[action] || action
}

onMounted(async () => {
  await load()
  timer = window.setInterval(load, 15000)
})

onUnmounted(() => {
  if (timer) window.clearInterval(timer)
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('文件夹监控') }}</h1>
        <p class="page-subtitle">{{ t('轮询监控目录，把新落盘的模型目录自动导入模型库，并按需排队转换。') }}</p>
      </div>
      <div class="page-actions">
        <button class="btn" :disabled="saving" @click="toggleEnabled">
          <Icon :name="enabled ? 'pause' : 'play'" :size="13" /> {{ enabled ? t('暂停监控') : t('启用监控') }}
        </button>
        <button class="btn btn-primary" :disabled="scanning" @click="scan(false)">
          <span v-if="scanning" class="spinner"></span>
          <Icon v-else name="refresh" :size="13" /> {{ t('立即扫描') }}
        </button>
      </div>
    </div>

    <div class="grid grid-4" style="margin-bottom: 14px">
      <div class="card stat-card" :class="{ 'stat-accent': enabled }">
        <div class="stat-top"><span>{{ t('监控状态') }}</span><Icon name="activity" :size="13" /></div>
        <div><div class="stat-value">{{ enabled ? t('运行中') : t('未启用') }}</div><div class="stat-note">{{ t('每 {0} 秒扫描一次', [status?.intervalSec || interval]) }}</div></div>
      </div>
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('监控目录') }}</span><Icon name="folder" :size="13" /></div>
        <div><div class="stat-value">{{ (status?.folders || []).length }}</div><div class="stat-note">{{ t('{0} 个目录可访问', [(status?.folders || []).filter((item: any) => item.exists).length]) }}</div></div>
      </div>
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('已知模型') }}</span><Icon name="models" :size="13" /></div>
        <div><div class="stat-value">{{ (status?.folders || []).reduce((sum: number, item: any) => sum + (item.known || 0), 0) }}</div><div class="stat-note">{{ t('记录过签名的目录') }}</div></div>
      </div>
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('上次扫描') }}</span><Icon name="clock" :size="13" /></div>
        <div><div class="stat-value" style="font-size: 18px">{{ status?.lastScanAt ? relativeTime(status.lastScanAt) : '—' }}</div><div class="stat-note">{{ status?.lastScanDurationMs ? t('耗时 {0} ms', [status.lastScanDurationMs]) : t('尚未扫描') }}</div></div>
      </div>
    </div>

    <div class="split-panel" style="margin-bottom: 14px">
      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">{{ t('监控设置') }}</div>
            <div class="card-subtitle">{{ t('模型目录需包含 config.json 与至少一个 .safetensors 文件') }}</div>
          </div>
        </div>
        <div class="card-body">
          <div class="field">
            <label class="field-label">{{ t('监控目录（每行一个绝对路径）') }}</label>
            <textarea v-model="foldersText" class="input mono" rows="4" :placeholder="t('例如 D:\\\\models\\\\incoming')"></textarea>
            <button class="btn btn-sm" style="margin-top: 8px" @click="dirPickerOpen = true"><Icon name="folder" :size="12" /> {{ t('浏览并添加目录') }}</button>
            <div class="field-hint">{{ t('只扫描目录的第一层子目录；已处理过的目录按“大小 + 修改时间”签名去重。') }}</div>
          </div>
          <div class="field-row">
            <div class="field">
              <label class="field-label">{{ t('扫描间隔（秒）') }}</label>
              <input v-model.number="interval" class="input mono" type="number" min="5" max="3600" />
            </div>
            <div class="field">
              <label class="field-label">{{ t('自动转换') }}</label>
              <label class="option-card" style="padding: 8px 10px">
                <input v-model="autoConvert" type="checkbox" />
                <div><div class="option-name">{{ t('导入后自动排队转换') }}</div><div class="option-desc">{{ t('使用下方预设的参数创建任务') }}</div></div>
              </label>
            </div>
          </div>
          <div class="field">
            <label class="field-label">{{ t('转换预设') }}</label>
            <select v-model="presetId" class="select" :disabled="!autoConvert">
              <option value="">{{ t('未选择（仅导入）') }}</option>
              <option v-for="profile in profiles" :key="profile.id" :value="profile.id">{{ t(profile.name) }}</option>
            </select>
            <div v-if="autoConvert && !presetId" class="field-hint">{{ t('未选择预设时不会自动转换，只导入模型库。') }}</div>
            <div v-else-if="activePreset" class="field-hint">
              {{ t('输出精度 {0} · 量化 {1} · 线程 {2}', [activePreset.params?.outType || 'f16', activePreset.params?.quantType || t('不量化'), activePreset.params?.threads || t('默认')]) }}
            </div>
          </div>
          <div class="btn-row" style="margin-top: 12px">
            <button class="btn btn-primary" :disabled="saving" @click="saveSettings">
              <span v-if="saving" class="spinner"></span>
              <Icon v-else name="check" :size="13" /> {{ t('保存监控设置') }}
            </button>
            <button class="btn" :disabled="scanning" @click="scan(true)">
              <Icon name="retry" :size="13" /> {{ t('重新导入全部') }}
            </button>
            <span class="faint tiny">{{ t('“重新导入全部”会忽略签名，对已有目录重新扫描与登记。') }}</span>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div>
            <div class="card-title">{{ t('扫描概况') }}</div>
            <div class="card-subtitle">{{ t('来自后端最近一次扫描') }}</div>
          </div>
        </div>
        <div class="card-body">
          <div v-if="status?.lastError" class="notice warning" style="margin-bottom: 10px">
            <Icon name="warning" :size="14" class="notice-icon" />{{ status.lastError }}
          </div>
          <div v-if="scanResult" class="notice info" style="margin-bottom: 10px">
            <Icon name="info" :size="14" class="notice-icon" />
            <div>
              {{ t('扫描完成：导入 {0} · 跳过 {1} · 排队 {2} · 失败 {3}', [scanResult.imported, scanResult.skipped, scanResult.queued, scanResult.failed]) }}
              <div v-for="error in (scanResult.errors || []).slice(0, 3)" :key="error.path" class="faint tiny">{{ error.path }} · {{ error.message }}</div>
            </div>
          </div>
          <div v-for="folder in (status?.folders || [])" :key="folder.path" class="well" style="margin-bottom: 8px">
            <div class="well-row"><span class="mono truncate" :title="folder.path">{{ folder.path }}</span>
              <span class="badge no-dot" :class="folder.exists ? 'success' : 'danger'">{{ folder.exists ? t('可访问') : t('不存在') }}</span></div>
            <div class="well-row"><span class="faint tiny">{{ t('已记录 {0} 个目录', [folder.known]) }}</span><span class="faint tiny">{{ folder.lastEventAt ? formatTime(folder.lastEventAt) : t('还没有事件') }}</span></div>
          </div>
          <EmptyState v-if="!(status?.folders || []).length" icon="folder" :title="t('尚未配置监控目录')" :copy="t('在左侧填入一个或多个目录，保存后即可开始监控。')" />
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-header">
        <div><div class="card-title">{{ t('事件日志') }}</div><div class="card-subtitle">{{ t('导入、转换入队与失败的完整记录') }}</div></div>
        <div class="card-header-actions"><span class="tag">{{ events.length }} EVENTS</span></div>
      </div>
      <div v-if="!events.length" class="empty-state" style="padding: 32px">
        <div class="empty-title">{{ t('还没有监控事件') }}</div>
        <div class="empty-copy">{{ t('启用监控或点击“立即扫描”后，事件会出现在这里。') }}</div>
      </div>
      <div v-else class="table-wrap">
        <table class="table">
          <thead><tr><th>{{ t('动作') }}</th><th>{{ t('目录') }}</th><th>{{ t('详情') }}</th><th>{{ t('时间') }}</th><th></th></tr></thead>
          <tbody>
            <tr v-for="event in events" :key="event.id">
              <td><span class="badge no-dot" :class="actionTone(event.action)">{{ actionLabel(event.action) }}</span></td>
              <td class="mono tiny" style="max-width: 300px"><div class="truncate" :title="event.path">{{ event.path }}</div></td>
              <td class="small">{{ event.detail || '—' }}</td>
              <td class="mono tiny">{{ formatTime(event.createdAt) }}</td>
              <td>
                <div class="table-actions">
                  <router-link v-if="event.modelId" :to="{ path: '/models', query: { versions: event.modelId } }" class="icon-btn" :title="t('查看版本树')"><Icon name="layers" :size="12" /></router-link>
                  <router-link v-if="event.taskId" :to="{ path: '/tasks', query: { task: event.taskId } }" class="icon-btn" :title="t('查看任务')"><Icon name="tasks" :size="12" /></router-link>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
    <DirectoryPicker v-model="dirPickerOpen" @select="addWatchedFolder" />

  </div>
</template>
