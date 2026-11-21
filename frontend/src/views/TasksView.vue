<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, formatDuration, formatTime, humanBytes } from '../api/client'
import type { LogEntry, Task } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import AppModal from '../components/AppModal.vue'
import ProgressBar from '../components/ProgressBar.vue'
import LogConsole from '../components/LogConsole.vue'
import EmptyState from '../components/EmptyState.vue'

const app = useAppStore()
const tasks = ref<Task[]>([])
const queue = ref<Record<string, any>>({})
const loading = ref(true)
const status = ref('ALL')
const search = ref('')
const concurrency = ref(1)
const savingConcurrency = ref(false)
const selected = ref<Set<string>>(new Set())
const detailOpen = ref(false)
const detailTask = ref<Task | null>(null)
const detailLogs = ref<LogEntry[]>([])
const detailLogUrl = ref('')
const detailResources = ref<any>(null)
let timer: number | undefined

const tabs = [
  { id: 'ALL', label: t('全部') },
  { id: 'RUNNING', label: t('运行中') },
  { id: 'QUEUED', label: t('等待中') },
  { id: 'SUCCEEDED', label: t('已完成') },
  { id: 'FAILED', label: t('失败') }
]

const filtered = computed(() => {
  const needle = search.value.trim().toLowerCase()
  return tasks.value.filter((task) => {
    if (status.value !== 'ALL' && task.status !== status.value) return false
    if (needle && !`${task.modelName} ${task.id} ${task.outputPath}`.toLowerCase().includes(needle)) return false
    return true
  })
})

const allSelected = computed(() => filtered.value.length > 0 && filtered.value.every((task) => selected.value.has(task.id)))

async function load() {
  try {
    const [list, queueData] = await Promise.all([
      api.get<Task[]>('/api/v1/conversions?limit=300'),
      api.get<Record<string, any>>('/api/v1/queue')
    ])
    tasks.value = list
    queue.value = queueData
    concurrency.value = queueData.maxConcurrent || 1
  } catch (error) {
    app.fromError(error, t('加载任务失败'))
  } finally {
    loading.value = false
  }
}

async function saveConcurrency() {
  savingConcurrency.value = true
  try {
    await api.put('/api/v1/queue/concurrency', { maxConcurrent: Number(concurrency.value) })
    app.toast('success', t('并发数已更新'), t('调度器会立即应用新限制'))
    await load()
  } catch (error) {
    app.fromError(error, t('更新并发数失败'))
  } finally {
    savingConcurrency.value = false
  }
}

async function action(task: Task, name: 'cancel' | 'pause' | 'resume' | 'retry') {
  try {
    const result = await api.post<Task>(`/api/v1/conversions/${task.id}/${name}`)
    const actionLabel = name === 'cancel' ? t('请求取消') : name === 'pause' ? t('暂停')
      : name === 'resume' ? t('恢复') : t('重新创建')
    app.toast('success', t('任务已{0}', [actionLabel]))
    if (name === 'retry' && result?.id) app.toast('info', t('新任务已创建'), result.id)
    await load()
  } catch (error) {
    app.fromError(error, t('任务操作失败'))
  }
}

async function removeTask(task: Task) {
  if (!window.confirm(t('删除任务记录 {0}？输出 GGUF 文件不会被删除。', [task.id]))) return
  try {
    await api.delete(`/api/v1/conversions/${task.id}?logs=true`)
    await load()
  } catch (error) {
    app.fromError(error, t('删除任务失败'))
  }
}

async function addPriority(task: Task, delta: number) {
  try {
    await api.patch(`/api/v1/conversions/${task.id}`, { priority: (task.priority || 0) + delta })
    await load()
  } catch (error) {
    app.fromError(error, t('调整优先级失败'))
  }
}

async function openDetail(task: Task) {
  detailTask.value = task
  detailOpen.value = true
  detailLogs.value = []
  detailLogUrl.value = `/api/v1/conversions/${task.id}/logs/download`
  detailResources.value = null
  try {
    const payload = await api.get<{ items: LogEntry[] }>(`/api/v1/conversions/${task.id}/logs?limit=3000`)
    detailLogs.value = payload.items || []
  } catch (error) {
    app.fromError(error, t('读取日志失败'))
  }
  try {
    detailResources.value = await api.get<any>(`/api/v1/conversions/${task.id}/resources`)
  } catch {
    detailResources.value = null
  }
}

async function refreshDetailLogs() {
  if (!detailTask.value) return
  try {
    const payload = await api.get<{ items: LogEntry[] }>(`/api/v1/conversions/${detailTask.value.id}/logs?limit=3000`)
    detailLogs.value = payload.items || []
  } catch (error) {
    app.fromError(error, t('刷新日志失败'))
  }
}

function toggleSelect(id: string) {
  const next = new Set(selected.value)
  if (next.has(id)) next.delete(id)
  else next.add(id)
  selected.value = next
}

function toggleAll() {
  selected.value = allSelected.value ? new Set() : new Set(filtered.value.map((item) => item.id))
}

async function batchAction(kind: 'cancel' | 'delete') {
  const items = tasks.value.filter((task) => selected.value.has(task.id))
  if (!items.length) return
  if (kind === 'delete' && !window.confirm(t('删除 {0} 条任务记录？输出文件不会被删除。', [items.length]))) return
  const results = await Promise.allSettled(items.map((task) => {
    if (kind === 'cancel') return api.post(`/api/v1/conversions/${task.id}/cancel`)
    return api.delete(`/api/v1/conversions/${task.id}?logs=true`)
  }))
  const failed = results.filter((item) => item.status === 'rejected').length
  app.toast(failed ? 'warning' : 'success', kind === 'cancel' ? t('批量取消请求已发送') : t('批量删除完成'), failed ? t('{0} 个操作失败', [failed]) : t('{0} 个任务', [items.length]))
  selected.value = new Set()
  await load()
}

function onRealtime(event: Event) {
  const payload = (event as CustomEvent).detail as Record<string, any>
  if (payload.type?.startsWith('task.')) {
    if (detailOpen.value && payload.taskId === detailTask.value?.id && payload.type === 'task.log' && payload.log) {
      detailLogs.value = [...detailLogs.value, payload.log]
    }
    load()
  }
}

function tone(task: Task) {
  if (task.status === 'SUCCEEDED') return 'success'
  if (task.status === 'FAILED') return 'danger'
  if (task.status === 'RUNNING') return 'active'
  if (task.status === 'PAUSED') return 'warning'
  return 'muted'
}

onMounted(async () => {
  await load()
  timer = window.setInterval(load, 3500)
  window.addEventListener('mf:event', onRealtime)
})
onUnmounted(() => {
  if (timer) window.clearInterval(timer)
  window.removeEventListener('mf:event', onRealtime)
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('批量任务') }}</h1>
        <p class="page-subtitle">{{ t('单个调度器按优先级和并发上限运行任务，避免多个大模型同时争抢内存。') }}</p>
      </div>
      <div class="page-actions">
        <router-link to="/convert" class="btn btn-primary"><Icon name="plus" :size="13" /> {{ t('新建转换') }}</router-link>
      </div>
    </div>

    <div class="grid grid-4" style="margin-bottom: 14px">
      <div class="card stat-card"><div class="stat-top"><span>{{ t('运行中') }}</span><Icon name="activity" :size="13" /></div><div><div class="stat-value" style="color: var(--accent)">{{ queue.running || 0 }}</div><div class="stat-note">{{ t('最多 {0} 个并发', [queue.maxConcurrent || 1]) }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('等待 / 暂停') }}</span><Icon name="clock" :size="13" /></div><div><div class="stat-value">{{ (queue.queued || 0) + (queue.paused || 0) }}</div><div class="stat-note">{{ t('{0} 个任务被暂停', [queue.paused || 0]) }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('已完成') }}</span><Icon name="check" :size="13" /></div><div><div class="stat-value">{{ queue.succeeded || 0 }}</div><div class="stat-note">{{ t('成功产物可在结果中下载') }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('失败 / 取消') }}</span><Icon name="warning" :size="13" /></div><div><div class="stat-value">{{ queue.failed || 0 }}<span class="faint" style="font-size: 15px"> / {{ queue.canceled || 0 }}</span></div><div class="stat-note">{{ t('失败任务保留完整日志与中间文件') }}</div></div></div>
    </div>

    <div class="toolbar">
      <button v-for="tab in tabs" :key="tab.id" class="chip" :class="{ active: status === tab.id }" @click="status = tab.id">{{ tab.label }}</button>
      <input v-model="search" class="input" :placeholder="t('搜索模型、任务 ID 或输出路径')" />
      <div class="toolbar-spacer"></div>
      <div style="display: flex; align-items: center; gap: 6px">
        <span class="small muted">{{ t('最大并发') }}</span>
        <input v-model.number="concurrency" type="number" min="1" max="8" class="input" style="width: 64px; height: 30px" />
        <button class="btn btn-sm" :disabled="savingConcurrency" @click="saveConcurrency">{{ t('应用') }}</button>
      </div>
    </div>

    <div v-if="selected.size" class="notice info" style="margin-bottom: 10px; align-items: center">
      <Icon name="tasks" :size="14" class="notice-icon" />
      <span>{{ t('已选择 {0} 个任务', [selected.size]) }}</span>
      <button class="btn btn-sm" style="margin-left: auto" @click="batchAction('cancel')">{{ t('批量取消') }}</button>
      <button class="btn btn-sm btn-danger" @click="batchAction('delete')">{{ t('批量删除记录') }}</button>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取任务队列…') }}</div>
    <div v-else-if="!filtered.length" class="card"><EmptyState icon="tasks" :title="t('当前筛选下没有任务')" copy="创建转换任务后，可以在这里暂停排队任务、调整优先级、取消进程或查看历史日志。" /></div>
    <div v-else class="card">
      <div class="table-wrap">
        <table class="table">
          <thead>
            <tr>
              <th style="width: 32px"><input type="checkbox" :checked="allSelected" @change="toggleAll" /></th>
              <th>{{ t('模型 / 任务') }}</th>
              <th>{{ t('状态') }}</th>
              <th style="min-width: 150px">{{ t('阶段与进度') }}</th>
              <th>{{ t('量化') }}</th>
              <th>{{ t('优先级') }}</th>
              <th>{{ t('耗时') }}</th>
              <th>{{ t('创建时间') }}</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="task in filtered" :key="task.id">
              <td><input type="checkbox" :checked="selected.has(task.id)" @change="toggleSelect(task.id)" /></td>
              <td>
                <button class="strong truncate" style="background: none; border: none; padding: 0; cursor: pointer; max-width: 220px; text-align: left" @click="openDetail(task)">{{ task.modelName }}</button>
                <div class="faint mono tiny">{{ task.id }}</div>
              </td>
              <td><span class="badge" :class="tone(task)">{{ t(task.stageLabel || task.status) }}</span></td>
              <td>
                <ProgressBar :value="task.progress" :mode="task.progressMode" :danger="task.status === 'FAILED'" />
                <div class="faint tiny" style="margin-top: 3px">{{ t(task.stageDetail || task.stageLabel || task.stage) }}</div>
              </td>
              <td><span class="tag">{{ task.params?.quantType || task.params?.outType || '—' }}</span></td>
              <td>
                <div style="display: flex; align-items: center; gap: 2px">
                  <span class="mono tiny">{{ task.priority }}</span>
                  <button v-if="task.status === 'QUEUED' || task.status === 'PAUSED'" class="icon-btn" :title="t('提高优先级')" @click="addPriority(task, 10)">+</button>
                </div>
              </td>
              <td class="mono small">{{ formatDuration(task.elapsedMs) }}</td>
              <td class="mono tiny">{{ formatTime(task.createdAt) }}</td>
              <td>
                <div class="table-actions">
                  <button v-if="task.status === 'QUEUED'" class="icon-btn" :title="t('暂停')" @click="action(task, 'pause')"><Icon name="pause" :size="12" /></button>
                  <button v-if="task.status === 'PAUSED'" class="icon-btn" :title="t('恢复')" @click="action(task, 'resume')"><Icon name="play" :size="12" /></button>
                  <button v-if="['RUNNING','QUEUED'].includes(task.status)" class="icon-btn" :title="t('取消')" @click="action(task, 'cancel')"><Icon name="stop" :size="12" /></button>
                  <button v-if="['FAILED','CANCELED'].includes(task.status)" class="icon-btn" :title="t('重试')" @click="action(task, 'retry')"><Icon name="retry" :size="12" /></button>
                  <button class="icon-btn" :title="t('日志')" @click="openDetail(task)"><Icon name="terminal" :size="12" /></button>
                  <button v-if="['SUCCEEDED','FAILED','CANCELED'].includes(task.status)" class="icon-btn" :title="t('删除记录')" @click="removeTask(task)"><Icon name="trash" :size="12" /></button>
                </div>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <AppModal v-model="detailOpen" :title="detailTask?.modelName || t('任务详情')" :subtitle="detailTask?.id" wide>
      <template v-if="detailTask">
        <div class="grid grid-2" style="margin-bottom: 12px">
          <div class="well">
            <div class="well-row"><span>{{ t('状态') }}</span><span><span class="badge" :class="tone(detailTask)">{{ t(detailTask.stageLabel || detailTask.status) }}</span></span></div>
            <div class="well-row"><span>{{ t('量化') }}</span><span class="mono">{{ detailTask.params?.quantType || detailTask.params?.outType || '—' }}</span></div>
            <div class="well-row"><span>{{ t('已用时间') }}</span><span class="mono">{{ formatDuration(detailTask.elapsedMs) }}</span></div>
            <div class="well-row"><span>{{ t('输入') }}</span><span class="mono" style="max-width: 260px; overflow: hidden; text-overflow: ellipsis">{{ detailTask.inputPath }}</span></div>
          </div>
          <div class="well">
            <div class="well-row"><span>{{ t('输出') }}</span><span class="mono" style="max-width: 260px; overflow: hidden; text-overflow: ellipsis">{{ t('{0}', [detailTask.outputPath || t('尚未生成')]) }}</span></div>
            <div class="well-row"><span>{{ t('体积') }}</span><span class="mono">{{ humanBytes(detailTask.outputSize) }}</span></div>
            <div class="well-row"><span>SHA256</span><span class="mono" style="max-width: 260px; overflow: hidden; text-overflow: ellipsis">{{ detailTask.sha256 || '—' }}</span></div>
            <div class="well-row"><span>{{ t('进度来源') }}</span><span class="mono">{{ detailTask.progressMode }}</span></div>
          </div>
        </div>
        <div v-if="detailResources?.sampled" class="grid grid-4" style="margin-top: 12px">
          <div class="card stat-card"><div class="stat-top"><span>{{ t('CPU 时间') }}</span><Icon name="activity" :size="13" /></div><div><div class="stat-value">{{ formatDuration(detailResources.cpuMs) }}</div><div class="stat-note">{{ t('占墙钟 {0}%', [detailResources.cpuPercent]) }}</div></div></div>
          <div class="card stat-card"><div class="stat-top"><span>{{ t('峰值内存') }}</span><Icon name="disk" :size="13" /></div><div><div class="stat-value">{{ humanBytes(detailResources.peakMemory) }}</div><div class="stat-note">{{ t('进程峰值工作集') }}</div></div></div>
          <div class="card stat-card"><div class="stat-top"><span>{{ t('磁盘读取') }}</span><Icon name="download" :size="13" /></div><div><div class="stat-value">{{ humanBytes(detailResources.readBytes) }}</div><div class="stat-note">{{ t('子进程累计 IO') }}</div></div></div>
          <div class="card stat-card"><div class="stat-top"><span>{{ t('磁盘写入') }}</span><Icon name="arrow" :size="13" /></div><div><div class="stat-value">{{ humanBytes(detailResources.writeBytes) }}</div><div class="stat-note">{{ t('子进程累计 IO') }}</div></div></div>
        </div>
        <div v-if="detailResources?.stages?.length" class="faint tiny" style="margin-top: 6px">
          <span v-for="stage in detailResources.stages" :key="stage.stage" style="margin-right: 14px">
            {{ t(stage.stageLabel) }} · {{ formatDuration(stage.durationMs) }} · {{ humanBytes(stage.peakMemory) }}
          </span>
        </div>
        <ProgressBar :value="detailTask.progress" :mode="detailTask.progressMode" :label="detailTask.stageDetail" :meta="detailTask.progress >= 0 ? detailTask.progress + '%' : 'indeterminate'" />
        <div v-if="detailTask.errorMessage" class="notice danger" style="margin-top: 10px">
          <Icon name="warning" :size="14" class="notice-icon" />
          <div><strong>{{ detailTask.errorCode }}</strong><br />{{ detailTask.errorMessage }}<div v-if="detailTask.suggestion" class="faint tiny">{{ detailTask.suggestion }}</div></div>
        </div>
        <div style="display: flex; align-items: center; gap: 8px; margin: 14px 0 8px">
          <span class="section-label" style="margin: 0">{{ t('完整日志') }}</span>
          <button class="btn btn-sm" style="margin-left: auto" @click="refreshDetailLogs"><Icon name="refresh" :size="12" /> {{ t('刷新') }}</button>
          <a v-if="detailTask.status === 'SUCCEEDED'" class="btn btn-sm btn-primary" :href="`/api/v1/conversions/${detailTask.id}/download`"><Icon name="download" :size="12" /> {{ t('下载产物') }}</a>
        </div>
        <LogConsole :entries="detailLogs" :height="380" :title="detailTask.id + '.log'" :download-url="detailLogUrl" @clear="detailLogs = []" />
      </template>
    </AppModal>
  </div>
</template>
