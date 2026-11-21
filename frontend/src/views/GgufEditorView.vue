<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { useRoute } from 'vue-router'
import { api, humanBytes } from '../api/client'
import type { Task } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import EmptyState from '../components/EmptyState.vue'

const app = useAppStore()
const route = useRoute()
const completed = ref<Task[]>([])
const selectedTaskId = ref('')
const manualPath = ref('')
const loading = ref(false)
const info = ref<Record<string, any> | null>(null)
const tab = ref<'metadata' | 'tensors' | 'edit'>('metadata')

const path = computed(() => manualPath.value.trim() || completed.value.find((item) => item.id === selectedTaskId.value)?.outputPath || '')
const metadataEntries = computed(() => Object.entries(info.value?.metadata || {}))
const tensors = computed(() => info.value?.tensors || [])

async function loadTasks() {
  try {
    completed.value = await api.get<Task[]>('/api/v1/conversions?status=SUCCEEDED&limit=200')
    if (completed.value.length) selectedTaskId.value = completed.value[0].id
  } catch (error) {
    app.fromError(error, t('加载 GGUF 列表失败'))
  }
}

async function inspect() {
  if (!path.value) {
    app.toast('warning', t('请选择或输入 GGUF 路径'))
    return
  }
  loading.value = true
  info.value = null
  try {
    info.value = await api.post('/api/v1/gguf/inspect', { path: path.value })
    if (!info.value?.valid) app.toast('warning', t('GGUF 结构验证失败'), (info.value?.errors || []).join('；'))
  } catch (error) {
    app.fromError(error, t('读取 GGUF 失败'))
  } finally {
    loading.value = false
  }
}

function formatValue(value: any): string {
  if (value === null || value === undefined) return '—'
  if (typeof value === 'string') return value.length > 180 ? value.slice(0, 180) + '…' : value
  if (typeof value === 'boolean' || typeof value === 'number') return String(value)
  if (Array.isArray(value)) return JSON.stringify(value)
  if (value.__array) return `[array · ${value.count} items] ` + JSON.stringify(value.preview).slice(0, 160)
  return JSON.stringify(value).slice(0, 180)
}

onMounted(async () => {
  await loadTasks()
  const queryPath = typeof route.query.path === 'string' ? route.query.path : ''
  if (queryPath) {
    manualPath.value = queryPath
    await inspect()
  }
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('GGUF 检查器') }}</h1>
        <p class="page-subtitle">{{ t('读取真实 GGUF 头、元数据与张量信息。当前 C++ 版本只读，避免不安全地重写文件。') }}</p>
      </div>
      <div class="page-actions">
        <span class="badge no-dot" :class="info?.valid ? 'success' : 'muted'">{{ t('{0}', [info ? (info.valid ? t('结构有效') : t('结构无效')) : t('未检查')]) }}</span>
      </div>
    </div>

    <div class="card" style="margin-bottom: 14px">
      <div class="card-body">
        <div class="field-row" style="align-items: end">
          <div class="field" style="margin: 0">
            <label class="field-label">{{ t('已完成任务') }}</label>
            <select v-model="selectedTaskId" class="select">
              <option value="">{{ t('手动输入路径') }}</option>
              <option v-for="task in completed" :key="task.id" :value="task.id">{{ task.modelName }} · {{ task.params?.quantType || task.params?.outType }}</option>
            </select>
          </div>
          <div class="field" style="margin: 0">
            <label class="field-label">{{ t('GGUF 路径') }}</label>
            <input v-model="manualPath" class="input mono" :placeholder="path || 'C:\\output\\model.gguf'" />
          </div>
          <button class="btn btn-primary" style="height: 35px" :disabled="loading || !path" @click="inspect">
            <span v-if="loading" class="spinner"></span><Icon v-else name="search" :size="13" /> {{ t('检查文件') }}
          </button>
        </div>
        <div v-if="completed.length && !manualPath" class="field-hint">{{ t('将读取：') }}<span class="mono">{{ path }}</span></div>
      </div>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取 GGUF 头部与元数据…') }}</div>
    <div v-else-if="!info" class="card">
      <EmptyState icon="gguf" :title="t('检查一个 GGUF 文件')" copy="选择已完成任务或输入本地路径。检查器只读取头部、元数据和张量索引，不载入完整权重。">
        <button class="btn" :disabled="!path" @click="inspect">{{ t('开始检查') }}</button>
      </EmptyState>
    </div>

    <template v-else>
      <div class="grid grid-4" style="margin-bottom: 14px">
        <div class="card stat-card"><div class="stat-top"><span>{{ t('GGUF 版本') }}</span><Icon name="gguf" :size="13" /></div><div><div class="stat-value">{{ info.version }}</div><div class="stat-note">{{ t('规范版本') }}</div></div></div>
        <div class="card stat-card"><div class="stat-top"><span>{{ t('张量数量') }}</span><Icon name="layers" :size="13" /></div><div><div class="stat-value">{{ info.tensorCount }}</div><div class="stat-note">{{ t('{0} 个元数据字段', [info.metadataCount]) }}</div></div></div>
        <div class="card stat-card"><div class="stat-top"><span>{{ t('文件体积') }}</span><Icon name="disk" :size="13" /></div><div><div class="stat-value" style="font-size: 20px">{{ humanBytes(info.fileSize) }}</div><div class="stat-note">{{ t('磁盘实际大小') }}</div></div></div>
        <div class="card stat-card"><div class="stat-top"><span>{{ t('架构 / 量化') }}</span><Icon name="analyzer" :size="13" /></div><div><div class="stat-value" style="font-size: 18px">{{ info.architecture || '—' }}</div><div class="stat-note">{{ t('{0}', [info.quantType || t('未知量化类型')]) }}</div></div></div>
      </div>

      <div v-if="info.errors?.length" class="notice danger" style="margin-bottom: 12px">
        <Icon name="warning" :size="14" class="notice-icon" />
        <div><div v-for="error in info.errors" :key="error">{{ error }}</div></div>
      </div>

      <div class="card">
        <div class="card-header">
          <div class="tabs" style="border: none; margin: 0; padding: 0">
            <button class="tab" :class="{ active: tab === 'metadata' }" @click="tab = 'metadata'">{{ t('元数据') }}</button>
            <button class="tab" :class="{ active: tab === 'tensors' }" @click="tab = 'tensors'">{{ t('张量') }}</button>
            <button class="tab" :class="{ active: tab === 'edit' }" @click="tab = 'edit'">{{ t('编辑能力') }}</button>
          </div>
          <div class="card-header-actions"><span class="tag">READ ONLY</span></div>
        </div>

        <div v-if="tab === 'metadata'" class="card-body flush table-wrap">
          <table class="table">
            <thead><tr><th style="width: 38%">{{ t('键') }}</th><th>{{ t('值') }}</th></tr></thead>
            <tbody>
              <tr v-for="[key, value] in metadataEntries" :key="key">
                <td class="mono strong" style="word-break: break-all">{{ key }}</td>
                <td class="mono tiny" style="word-break: break-all">{{ formatValue(value) }}</td>
              </tr>
              <tr v-if="!metadataEntries.length"><td colspan="2" class="muted">{{ t('没有元数据') }}</td></tr>
            </tbody>
          </table>
        </div>

        <div v-else-if="tab === 'tensors'" class="card-body flush table-wrap">
          <table class="table">
            <thead><tr><th>{{ t('张量名称') }}</th><th>{{ t('形状') }}</th><th>{{ t('类型 ID') }}</th><th>{{ t('偏移') }}</th></tr></thead>
            <tbody>
              <tr v-for="tensor in tensors" :key="tensor.name">
                <td class="mono tiny strong" style="word-break: break-all">{{ tensor.name }}</td>
                <td class="mono tiny">{{ Array.isArray(tensor.shape) ? tensor.shape.join(' × ') : tensor.shape }}</td>
                <td class="mono tiny">{{ tensor.type }}</td>
                <td class="mono tiny">{{ tensor.offset }}</td>
              </tr>
              <tr v-if="!tensors.length"><td colspan="4" class="muted">{{ t('当前仅保留最多 500 个张量用于展示。') }}</td></tr>
            </tbody>
          </table>
        </div>

        <div v-else class="card-body">
          <div class="notice warning">
            <Icon name="warning" :size="14" class="notice-icon" />
            <div>
              <strong>{{ t('元数据编辑在第一个 C++ 版本中未开放。') }}</strong>
              {{ t('GGUF 的文本元数据位于张量数据之前；修改字段长度会移动数据区偏移和对齐。安全实现必须先完整解析所有元数据类型、重写头部并重新校验张量偏移。为了避免损坏模型，ModelForge 当前只读，并明确返回') }} <span class="mono">GGUF_EDIT_READ_ONLY</span>。
            </div>
          </div>
          <div class="section-label">{{ t('建议的安全流程') }}</div>
          <div class="timeline">
            <div class="timeline-item"><span class="timeline-dot info"></span><div class="timeline-event">{{ t('复制原始 GGUF 到独立路径，保留校验值。') }}</div></div>
            <div class="timeline-item"><span class="timeline-dot info"></span><div class="timeline-event">{{ t('使用受支持的 GGUF 工具修改可安全编辑的文本元数据。') }}</div></div>
            <div class="timeline-item"><span class="timeline-dot info"></span><div class="timeline-event">{{ t('回到本页面重新检查魔数、版本、元数据与张量数量。') }}</div></div>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>
