<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, formatTime, relativeTime } from '../api/client'
import type { Model } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import EmptyState from '../components/EmptyState.vue'

interface HistoryItem {
  id: string
  modelId: string
  taskId: string
  event: string
  detail: string
  createdAt: string
}

const app = useAppStore()
const items = ref<HistoryItem[]>([])
const models = ref<Model[]>([])
const loading = ref(true)
const search = ref('')

const modelNames = computed(() => Object.fromEntries(models.value.map((item) => [item.id, item.name])))
const filtered = computed(() => {
  const needle = search.value.trim().toLowerCase()
  if (!needle) return items.value
  return items.value.filter((item) =>
    `${modelNames.value[item.modelId] || ''} ${item.event} ${item.detail} ${item.taskId}`.toLowerCase().includes(needle)
  )
})

function tone(event: string) {
  if (event.includes('FAILED')) return 'fail'
  if (event.includes('SUCCEEDED') || event.includes('MODEL_IMPORTED')) return 'success'
  if (event.includes('CANCELED')) return 'info'
  return 'info'
}

function eventLabel(event: string) {
  const map: Record<string, string> = {
    MODEL_IMPORTED: t('导入模型'),
    TASK_CREATED: t('创建任务'),
    TASK_SUCCEEDED: t('转换成功'),
    TASK_FAILED: t('转换失败'),
    TASK_CANCELED: t('任务取消')
  }
  return map[event] || event
}

async function load() {
  loading.value = true
  try {
    const [history, modelList] = await Promise.all([
      api.get<HistoryItem[]>('/api/v1/history?limit=300'),
      api.get<Model[]>('/api/v1/models?limit=500')
    ])
    items.value = history
    models.value = modelList
  } catch (error) {
    app.fromError(error, t('加载历史失败'))
  } finally {
    loading.value = false
  }
}

onMounted(load)
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('转换历史') }}</h1>
        <p class="page-subtitle">{{ t('导入、扫描、任务与失败事件形成可追溯时间线；详细日志保留在独立文件中。') }}</p>
      </div>
      <div class="page-actions">
        <button class="btn" @click="load"><Icon name="refresh" :size="13" /> {{ t('刷新') }}</button>
      </div>
    </div>

    <div class="grid grid-4" style="margin-bottom: 14px">
      <div class="card stat-card"><div class="stat-top"><span>{{ t('历史事件') }}</span><Icon name="history" :size="13" /></div><div><div class="stat-value">{{ items.length }}</div><div class="stat-note">{{ t('最近 300 条') }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('成功转换') }}</span><Icon name="check" :size="13" /></div><div><div class="stat-value">{{ items.filter(i => i.event === 'TASK_SUCCEEDED').length }}</div><div class="stat-note">{{ t('含全部历史任务') }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('失败') }}</span><Icon name="warning" :size="13" /></div><div><div class="stat-value">{{ items.filter(i => i.event === 'TASK_FAILED').length }}</div><div class="stat-note">{{ t('保留错误码与日志') }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('导入模型') }}</span><Icon name="models" :size="13" /></div><div><div class="stat-value">{{ items.filter(i => i.event === 'MODEL_IMPORTED').length }}</div><div class="stat-note">{{ t('模型库记录') }}</div></div></div>
    </div>

    <div class="toolbar">
      <div style="position: relative; width: 320px">
        <Icon name="search" :size="14" style="position: absolute; left: 10px; top: 10px; color: var(--faint)" />
        <input v-model="search" class="input" style="padding-left: 31px" :placeholder="t('搜索事件、模型或任务 ID')" />
      </div>
      <div class="toolbar-spacer"></div>
      <span class="tag">{{ filtered.length }} EVENTS</span>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取时间线…') }}</div>
    <div v-else-if="!filtered.length" class="card"><EmptyState icon="history" :title="t('暂无历史事件')" copy="导入模型或创建转换任务后，这里会形成完整时间线。" /></div>
    <div v-else class="card">
      <div class="card-body">
        <div class="timeline">
          <div v-for="item in filtered" :key="item.id" class="timeline-item">
            <span class="timeline-dot" :class="tone(item.event)"></span>
            <div style="display: flex; align-items: flex-start; gap: 12px">
              <div style="flex: 1; min-width: 0">
                <div class="timeline-event"><strong>{{ eventLabel(item.event) }}</strong> <span class="muted">· {{ modelNames[item.modelId] || item.modelId || 'unknown model' }}</span></div>
                <div class="small muted" style="margin-top: 3px; word-break: break-all">{{ item.detail }}</div>
                <div class="timeline-meta">
                  <span>{{ formatTime(item.createdAt) }}</span>
                  <span>{{ relativeTime(item.createdAt) }}</span>
                  <span v-if="item.taskId">TASK {{ item.taskId }}</span>
                </div>
              </div>
              <router-link v-if="item.taskId" :to="{ path: '/tasks' }" class="btn btn-sm">{{ t('查看任务') }}</router-link>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
