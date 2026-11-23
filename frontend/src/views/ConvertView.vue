<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { api, formatDuration, humanBytes, humanNumber } from '../api/client'
import type { EstimateOption, LogEntry, Model, Task } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import LogConsole from '../components/LogConsole.vue'
import ProgressBar from '../components/ProgressBar.vue'
import EmptyState from '../components/EmptyState.vue'

const route = useRoute()
const app = useAppStore()

const models = ref<Model[]>([])
const model = ref<Model | null>(null)
const estimates = ref<EstimateOption[]>([])
const loadingModels = ref(true)
const loadingModel = ref(false)
const starting = ref(false)
const advancedOpen = ref(false)
const preview = ref<Record<string, string> | null>(null)
const previewTab = ref<'convert' | 'quantize'>('convert')
const pendingPreset = ref<string>((route.query.preset as string) || '')
const appliedPreset = ref('')

const form = ref({
  modelId: '',
  outputDir: '',
  outputName: '',
  outType: 'f16',
  quantType: '',
  threads: 4,
  overwrite: false,
  keepIntermediate: false,
  keepTemp: false,
  useTempFile: false,
  noLazy: false,
  verify: true,
  deepVerify: false,
  computeSha256: true
})

const activeTask = ref<Task | null>(null)
const logs = ref<LogEntry[]>([])
const resultData = ref<Record<string, any> | null>(null)
const lastSeq = ref(0)
let pollTimer: number | undefined
let previewTimer: number | undefined
let disposed = false

const stages = computed(() => {
  const list = [
    { id: 'SCANNING', label: t('扫描模型') },
    { id: 'CHECKING', label: t('检查兼容性') },
    { id: 'LOADING_CONFIG', label: t('加载配置') },
    { id: 'CONVERTING', label: t('权重转换 / GGUF 写入') }
  ]
  if (form.value.quantType) list.push({ id: 'QUANTIZING', label: t('量化处理') })
  list.push({ id: 'VERIFYING', label: t('文件验证') }, { id: 'COMPLETED', label: t('转换完成') })
  return list
})

const stageIndex = computed(() => {
  if (!activeTask.value) return -1
  return stages.value.findIndex((item) => item.id === activeTask.value?.stage)
})

const finalEstimate = computed(() => {
  const target = form.value.quantType || form.value.outType.toUpperCase()
  return estimates.value.find((item) => item.type.toLowerCase() === target.toLowerCase()) || null
})

const availableQuants = computed(() => {
  const types = app.capabilities?.quantTypes || []
  const preferred = ['Q8_0', 'Q6_K', 'Q5_K_M', 'Q4_K_M', 'Q4_K', 'Q3_K_M', 'Q2_K', 'IQ4_XS', 'IQ3_M', 'IQ2_M']
  const sorted = [...types].filter((item) => !['F32', 'F16', 'BF16', 'COPY'].includes(item.toUpperCase()))
  return sorted.sort((a, b) => {
    const ai = preferred.indexOf(a)
    const bi = preferred.indexOf(b)
    if (ai === -1 && bi === -1) return a.localeCompare(b)
    if (ai === -1) return 1
    if (bi === -1) return -1
    return ai - bi
  })
})

const outTypes = computed(() => {
  const available = app.capabilities?.conversionTypes?.length ? app.capabilities.conversionTypes : ['f32', 'f16', 'bf16']
  return [
    { id: 'f16', label: 'F16', desc: t('16 位浮点 · 推荐中间精度') },
    { id: 'bf16', label: 'BF16', desc: t('Bfloat16 · 适合部分新模型') },
    { id: 'f32', label: 'F32', desc: t('32 位浮点 · 最大保真与体积') }
  ].filter((item) => available.includes(item.id))
})

const canStart = computed(() => Boolean(
  model.value && app.capabilities?.conversionReady && !starting.value &&
  (!activeTask.value || ['SUCCEEDED', 'FAILED', 'CANCELED'].includes(activeTask.value.status))
))

const progressLabel = computed(() => {
  if (!activeTask.value) return t('尚未开始')
  if (activeTask.value.progress < 0) return t('底层工具未提供精确百分比')
  if (activeTask.value.progressMode === 'measured') return t('实测进度')
  if (activeTask.value.progressMode === 'estimated') return t('估算进度 · 基于输出文件增长')
  return t('阶段进度')
})

const logUrl = computed(() => activeTask.value ? `/api/v1/conversions/${activeTask.value.id}/logs/download` : '')

async function applyPreset(profileId: string) {
  try {
    const profiles = await api.get<any[]>('/api/v1/quantization/profiles')
    const profile = profiles.find((item) => item.id === profileId)
    if (!profile) {
      app.toast('warning', t('预设不存在或已被删除'))
      pendingPreset.value = ''
      return
    }
    const params = profile.params || {}
    if (params.outType && ['f16', 'bf16', 'f32'].includes(String(params.outType))) {
      form.value.outType = String(params.outType)
    }
    const quant = String(params.quantType || '')
    if (quant && availableQuants.value.includes(quant)) form.value.quantType = quant
    else if (quant) app.toast('warning', t('预设中的量化类型当前不可用'), quant)
    if (params.threads !== undefined) form.value.threads = Number(params.threads) || 0
    if (typeof params.verify === 'boolean') form.value.verify = params.verify
    if (typeof params.computeSha256 === 'boolean') form.value.computeSha256 = params.computeSha256
    appliedPreset.value = profile.name
    app.toast('success', t('已套用预设'), profile.name)
    pendingPreset.value = ''
  } catch (error) {
    app.fromError(error, t('套用预设失败'))
    pendingPreset.value = ''
  }
}

async function loadModels() {
  loadingModels.value = true
  try {
    models.value = await api.get<Model[]>('/api/v1/models?limit=500')
    const queryId = typeof route.query.model === 'string' ? route.query.model : ''
    const initial = models.value.find((item) => item.id === queryId) || models.value[0]
    if (initial) {
      form.value.modelId = initial.id
      await selectModel(initial.id)
    }
  } catch (error) {
    app.fromError(error, t('加载模型列表失败'))
  } finally {
    loadingModels.value = false
  }
}

async function selectModel(id: string) {
  if (!id) return
  loadingModel.value = true
  try {
    model.value = await api.get<Model>(`/api/v1/models/${id}`)
    form.value.modelId = id
    if (!form.value.outputName || form.value.outputName === 'model') {
      form.value.outputName = model.value.name
    }
    form.value.outputDir = app.settings?.defaultOutputDir || form.value.outputDir
    form.value.threads = app.settings?.threads || form.value.threads
    const result = await api.get<{ options: EstimateOption[] }>(`/api/v1/models/${id}/estimates`)
    estimates.value = result.options || []
    const preferred = availableQuants.value.find((item) => item === 'Q4_K_M') || availableQuants.value[0] || ''
    form.value.quantType = preferred
    if (pendingPreset.value) await applyPreset(pendingPreset.value)
    schedulePreview()
  } catch (error) {
    app.fromError(error, t('读取模型失败'))
  } finally {
    loadingModel.value = false
  }
}

function taskParams() {
  return {
    modelId: form.value.modelId,
    inputPath: model.value?.path || '',
    outputDir: form.value.outputDir,
    outputName: form.value.outputName,
    outType: form.value.outType,
    quantType: form.value.quantType,
    threads: Number(form.value.threads) || 0,
    overwrite: form.value.overwrite,
    keepIntermediate: form.value.keepIntermediate,
    keepTemp: form.value.keepTemp,
    verify: form.value.verify,
    deepVerify: form.value.deepVerify,
    computeSha256: form.value.computeSha256,
    useTempFile: form.value.useTempFile,
    noLazy: form.value.noLazy
  }
}

function schedulePreview() {
  if (previewTimer) window.clearTimeout(previewTimer)
  previewTimer = window.setTimeout(refreshPreview, 380)
}

async function refreshPreview() {
  if (!form.value.modelId) return
  try {
    preview.value = await api.post<Record<string, string>>('/api/v1/conversions/preview', taskParams())
    previewTab.value = form.value.quantType ? 'quantize' : 'convert'
  } catch {
    preview.value = null
  }
}

async function startConversion() {
  if (!model.value) return
  starting.value = true
  resultData.value = null
  try {
    const task = await api.post<Task>('/api/v1/conversions', taskParams())
    setActiveTask(task)
    app.toast('success', t('转换任务已创建'), t('任务将在队列中按并发限制执行'))
  } catch (error) {
    app.fromError(error, t('无法创建转换任务'))
  } finally {
    starting.value = false
  }
}

function setActiveTask(task: Task) {
  activeTask.value = task
  if (!['SUCCEEDED', 'FAILED', 'CANCELED'].includes(task.status)) {
    logs.value = []
    lastSeq.value = 0
    loadLogs(true)
    ensurePoll()
  } else if (task.status === 'SUCCEEDED') {
    loadResult()
    loadLogs(true)
  }
}

async function loadTask(id: string) {
  try {
    const task = await api.get<Task>(`/api/v1/conversions/${id}`)
    activeTask.value = task
    if (task.status === 'SUCCEEDED') await loadResult()
  } catch {
    // Ignore transient fetch errors; the next poll retries.
  }
}

async function loadLogs(reset = false) {
  if (!activeTask.value) return
  try {
    const payload = await api.get<{ items: LogEntry[] }>(
      `/api/v1/conversions/${activeTask.value.id}/logs?afterSeq=${reset ? 0 : lastSeq.value}&limit=2000`
    )
    const incoming = payload.items || []
    if (reset) {
      logs.value = incoming
      lastSeq.value = 0
    } else {
      const existing = new Set(logs.value.map((item) => item.seq))
      logs.value = [...logs.value, ...incoming.filter((item) => !existing.has(item.seq))].slice(-4000)
    }
    for (const item of incoming) lastSeq.value = Math.max(lastSeq.value, item.seq)
  } catch {
    // Logs are optional during backend restarts.
  }
}

async function loadResult() {
  if (!activeTask.value) return
  try {
    const result = await api.get<Record<string, any>>(`/api/v1/conversions/${activeTask.value.id}/result`)
    resultData.value = result.ready ? result : null
  } catch {
    resultData.value = null
  }
}

function ensurePoll() {
  if (pollTimer) return
  pollTimer = window.setInterval(async () => {
    if (!activeTask.value || !['RUNNING', 'QUEUED', 'PAUSED'].includes(activeTask.value.status)) {
      if (pollTimer) window.clearInterval(pollTimer)
      pollTimer = undefined
      return
    }
    await loadTask(activeTask.value.id)
    await loadLogs(false)
  }, 1400)
}

async function cancelTask() {
  if (!activeTask.value) return
  try {
    await api.post(`/api/v1/conversions/${activeTask.value.id}/cancel`)
    app.toast('warning', t('已请求取消任务'), t('后端将终止子进程并恢复可能覆盖的旧文件'))
    await loadTask(activeTask.value.id)
  } catch (error) {
    app.fromError(error, t('取消失败'))
  }
}

function onRealtime(event: Event) {
  const payload = (event as CustomEvent).detail as Record<string, any>
  if (!activeTask.value) return
  if (payload.taskId !== activeTask.value.id) return
  if (payload.task) activeTask.value = payload.task as Task
  if (payload.type === 'task.log' && payload.log) {
    const entry = payload.log as LogEntry
    if (!logs.value.some((item) => item.seq === entry.seq)) {
      logs.value = [...logs.value, entry].slice(-4000)
      lastSeq.value = Math.max(lastSeq.value, entry.seq)
    }
  }
  if (payload.type === 'task.progress' && activeTask.value) {
    activeTask.value = {
      ...activeTask.value,
      progress: payload.progress,
      progressMode: payload.mode,
      stageDetail: payload.detail,
      elapsedMs: payload.elapsedMs
    }
  }
  if (payload.type === 'task.completed') {
    loadTask(activeTask.value.id)
    app.toast('success', t('转换完成'), t('GGUF 已通过结构验证'))
  }
  if (payload.type === 'task.failed') {
    loadTask(activeTask.value.id)
    app.fromError(new Error(payload.message || t('转换失败')), t('转换失败'))
  }
  if (payload.type === 'task.canceled') loadTask(activeTask.value.id)
}

async function copyText(text: string) {
  try {
    await navigator.clipboard.writeText(text)
    app.toast('success', t('已复制'))
  } catch {
    app.toast('error', t('复制失败'))
  }
}

watch(form, schedulePreview, { deep: true })
watch(() => form.value.modelId, (id, old) => { if (id && id !== old) selectModel(id) })

onMounted(async () => {
  await app.init()
  await loadModels()
  window.addEventListener('mf:event', onRealtime)
})

onUnmounted(() => {
  disposed = true
  window.removeEventListener('mf:event', onRealtime)
  if (pollTimer) window.clearInterval(pollTimer)
  if (previewTimer) window.clearTimeout(previewTimer)
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('模型转换') }}</h1>
        <p class="page-subtitle">{{ t('Safetensors 转换与 GGUF 量化分为两个阶段。进度来自真实输出文件增长。') }}</p>
      </div>
      <div class="page-actions">
        <span class="badge" :class="app.capabilities?.conversionReady ? 'success' : 'danger'"> {{ t('{0}', [app.capabilities?.conversionReady ? t('转换链路可用') : t('转换链路未就绪')]) }}</span>
        <router-link to="/tasks" class="btn">{{ t('任务队列') }}</router-link>
      </div>
    </div>

    <div v-if="loadingModels" class="loading-panel"><span class="spinner"></span> {{ t('正在加载模型与工具能力…') }}</div>
    <div v-else-if="!models.length" class="card">
      <EmptyState icon="convert" :title="t('暂无可转换模型')" copy="请先导入包含 config.json、完整 Safetensors 权重和 Tokenizer 的模型目录。">
        <router-link to="/models" class="btn btn-primary">{{ t('导入模型') }}</router-link>
      </EmptyState>
    </div>

    <div v-else class="convert-layout">
      <div class="card sticky-col">
        <div class="card-header"><div><div class="card-title">{{ t('源模型') }}</div><div class="card-subtitle">{{ t('文件只读，不会修改原目录') }}</div></div></div>
        <div class="card-body">
          <div class="field">
            <label class="field-label">{{ t('选择模型') }}</label>
            <select v-model="form.modelId" class="select">
              <option v-for="item in models" :key="item.id" :value="item.id">{{ item.name }}</option>
            </select>
          </div>

          <div v-if="loadingModel" class="loading-panel" style="min-height: 90px"><span class="spinner"></span></div>
          <template v-else-if="model">
            <div class="well" style="margin-bottom: 10px">
              <div class="well-row"><span>{{ t('架构') }}</span><span class="mono">{{ model.architecture }}</span></div>
              <div class="well-row"><span>{{ t('参数') }}</span><span class="mono">{{ humanNumber(model.parameterCount) }}</span></div>
              <div class="well-row"><span>{{ t('原始精度') }}</span><span class="mono">{{ model.dtype || '—' }}</span></div>
              <div class="well-row"><span>{{ t('总大小') }}</span><span class="mono">{{ humanBytes(model.totalSize) }}</span></div>
              <div class="well-row"><span>{{ t('分片 / 张量') }}</span><span class="mono">{{ model.shardCount }} / {{ model.tensorCount }}</span></div>
              <div class="well-row"><span>{{ t('完整性') }}</span><span class="badge no-dot" :class="model.status === 'READY' ? 'success' : 'danger'">{{ model.status }}</span></div>
            </div>
            <div class="faint tiny mono" style="word-break: break-all; margin-bottom: 10px">{{ model.path }}</div>

            <div class="section-label">{{ t('预估输出') }}</div>
            <div v-if="finalEstimate" class="well">
              <div class="well-row"><span>{{ t('目标方案') }}</span><span class="mono">{{ form.quantType || form.outType.toUpperCase() }}<span v-if="appliedPreset" class="tag" style="margin-left: 6px">{{ t('预设：{0}', [appliedPreset]) }}</span></span></div>
              <div class="well-row"><span>{{ t('预计体积') }}</span><span class="mono" style="color: var(--accent)">{{ humanBytes(finalEstimate.estimatedSize) }}</span></div>
              <div class="well-row"><span>{{ t('运行内存') }}</span><span class="mono">{{ humanBytes(finalEstimate.estimatedRuntimeMemory) }}</span></div>
              <div class="faint tiny" style="margin-top: 6px">{{ t('估算值，实际以输出文件为准') }}</div>
            </div>
          </template>

          <div v-if="!app.capabilities?.conversionReady" class="notice danger" style="margin-top: 12px">
            <Icon name="warning" :size="14" class="notice-icon" />
            <div>{{ t('转换依赖未就绪。') }}<router-link to="/settings">{{ t('检查设置') }}</router-link></div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header"><div><div class="card-title">{{ t('转换参数') }}</div><div class="card-subtitle">{{ t('普通配置 + 可展开高级选项') }}</div></div></div>
        <div class="card-body">
          <div class="section-label" style="margin-top: 0">{{ t('输出') }}</div>
          <div class="field-row">
            <div class="field">
              <label class="field-label">{{ t('输出文件名') }}</label>
              <input v-model="form.outputName" class="input" placeholder="model" />
              <div class="field-hint">{{ t('自动附加 .gguf 扩展名') }}</div>
            </div>
            <div class="field">
              <label class="field-label">{{ t('线程数') }}</label>
              <input v-model.number="form.threads" class="input" type="number" min="0" max="512" />
              <div class="field-hint">{{ t('0 表示使用系统默认线程') }}</div>
            </div>
          </div>
          <div class="field">
            <label class="field-label">{{ t('输出目录') }}</label>
            <input v-model="form.outputDir" class="input mono" placeholder="C:\output" />
            <div class="field-hint">{{ t('目录不存在时后端会自动创建；路径必须可写。') }}</div>
          </div>

          <div class="section-label">{{ t('第一阶段 · 中间精度') }}</div>
          <div class="option-list">
            <label v-for="item in outTypes" :key="item.id" class="option-card" :class="{ selected: form.outType === item.id }">
              <input v-model="form.outType" type="radio" :value="item.id" style="display: none" />
              <span class="option-radio"></span>
              <div><div class="option-name">{{ item.label }}</div><div class="option-desc">{{ item.desc }}</div></div>
              <div class="option-size">{{ humanBytes(estimates.find(e => e.type.toLowerCase() === item.id)?.estimatedSize) }}</div>
            </label>
          </div>

          <div class="section-label">{{ t('第二阶段 · 量化方案') }}</div>
          <select v-model="form.quantType" class="select" style="margin-bottom: 8px">
            <option value="">{{ t('不量化（仅输出 {0}）', [form.outType.toUpperCase()]) }}</option>
            <option v-for="quant in availableQuants" :key="quant" :value="quant">{{ t('{0} — {1}', [quant, t(estimates.find(e => e.type.toLowerCase() === quant.toLowerCase())?.description || 'llama.cpp 支持的量化类型')]) }}</option>
          </select>
          <div v-if="!availableQuants.length" class="notice warning">
            <Icon name="warning" :size="14" class="notice-icon" />
            <div>{{ t('当前未检测到 llama-quantize 的可用量化类型。仍可执行第一阶段 F16/BF16 转换。') }}</div>
          </div>
          <div v-else-if="form.quantType" class="notice info">
            <Icon name="info" :size="14" class="notice-icon" />
            <div>{{ t('{0}', [t(estimates.find(e => e.type.toLowerCase() === form.quantType.toLowerCase())?.description || '量化会先完成中间 GGUF，再调用 llama-quantize。')]) }}</div>
          </div>

          <div class="section-label">{{ t('行为') }}</div>
          <div class="grid grid-2" style="gap: 8px 14px">
            <label class="check"><input v-model="form.overwrite" type="checkbox" /><span>{{ t('覆盖已有输出') }}<small>{{ t('旧文件会先备份，失败时恢复') }}</small></span></label>
            <label class="check"><input v-model="form.keepIntermediate" type="checkbox" /><span>{{ t('保留中间 GGUF') }}<small>{{ t('便于复用或重新量化') }}</small></span></label>
            <label class="check"><input v-model="form.verify" type="checkbox" /><span>{{ t('转换后结构验证') }}<small>{{ t('校验 GGUF 魔数、版本与张量') }}</small></span></label>
            <label class="check"><input v-model="form.computeSha256" type="checkbox" /><span>{{ t('计算 SHA256') }}<small>{{ t('对大文件流式计算') }}</small></span></label>
          </div>

          <button class="btn btn-ghost btn-sm" style="margin-top: 14px" @click="advancedOpen = !advancedOpen"> {{ t('{0}', [advancedOpen ? t('收起高级设置') : t('展开高级设置')]) }}<Icon name="arrow" :size="11" />
          </button>
          <div v-if="advancedOpen" style="margin-top: 12px">
            <div class="grid grid-2" style="gap: 8px 14px">
              <label class="check"><input v-model="form.deepVerify" type="checkbox" /><span>{{ t('llama-cli 深度加载验证') }}<small>{{ t('会实际加载模型，可能耗时较长') }}</small></span></label>
              <label class="check"><input v-model="form.useTempFile" type="checkbox" /><span>{{ t('转换使用临时文件') }}<small>{{ t('降低内存峰值，可能增加磁盘 IO') }}</small></span></label>
              <label class="check"><input v-model="form.noLazy" type="checkbox" /><span>{{ t('禁用延迟计算') }}<small>{{ t('使用更多内存换取兼容性') }}</small></span></label>
              <label class="check"><input v-model="form.keepTemp" type="checkbox" /><span>{{ t('保留临时文件') }}<small>{{ t('用于故障诊断') }}</small></span></label>
            </div>
            <div class="notice info" style="margin-top: 11px">
              <Icon name="info" :size="14" class="notice-icon" />
              <div>{{ t('GPU 加速不适用于 convert_hf_to_gguf.py 与 llama-quantize 阶段；GPU 卸载参数仅用于模型测试与推理。ModelForge 不会传递未经验证的参数。') }}</div>
            </div>
          </div>

          <div class="section-label">{{ t('命令预览') }}</div>
          <div class="tabs" style="margin-bottom: 8px">
            <button class="tab" :class="{ active: previewTab === 'convert' }" @click="previewTab = 'convert'">{{ t('转换命令') }}</button>
            <button class="tab" :class="{ active: previewTab === 'quantize' }" @click="previewTab = 'quantize'">{{ t('量化命令') }}</button>
            <button class="tab" @click="copyText(preview?.[previewTab] || '')">{{ t('复制') }}</button>
          </div>
          <div class="code-block">{{ t('{0}', [preview?.[previewTab] || (previewTab === 'quantize' && !form.quantType ? t('未选择量化方案，不生成量化命令。') : t('正在生成安全命令预览…'))]) }}</div>

          <div class="btn-row" style="margin-top: 16px">
            <button class="btn btn-primary btn-lg" :disabled="!canStart" @click="startConversion">
              <span v-if="starting" class="spinner"></span>
              <Icon v-else name="play" :size="14" /> {{ t('{0}', [starting ? t('创建任务…') : activeTask && ['RUNNING','QUEUED'].includes(activeTask.status) ? t('创建另一个任务') : t('开始转换')]) }}</button>
            <button v-if="activeTask && ['RUNNING','QUEUED'].includes(activeTask.status)" class="btn btn-danger btn-lg" @click="cancelTask">
              <Icon name="stop" :size="13" /> {{ t('取消任务') }}
            </button>
          </div>
        </div>
      </div>

      <div class="sticky-col">
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">{{ t('任务监控') }}</div>
              <div class="card-subtitle">{{ t('{0}', [activeTask ? activeTask.id : t('等待创建任务')]) }}</div>
            </div>
            <div class="card-header-actions">
              <span v-if="activeTask" class="badge" :class="activeTask.status === 'SUCCEEDED' ? 'success' : activeTask.status === 'FAILED' ? 'danger' : activeTask.status === 'RUNNING' ? 'active' : 'muted'">{{ t(activeTask.stageLabel || activeTask.stage) }}</span>
            </div>
          </div>
          <div class="card-body">
            <div v-if="!activeTask" class="empty-state" style="padding: 24px 10px">
              <div class="empty-icon"><Icon name="activity" :size="20" /></div>
              <div class="empty-title">{{ t('监控面板待命') }}</div>
              <div class="empty-copy">{{ t('创建任务后，阶段、真实进程日志、文件增长估算与产物信息会显示在这里。') }}</div>
            </div>
            <template v-else>
              <div style="display: flex; align-items: baseline; gap: 8px; margin-bottom: 7px">
                <span class="mono" style="font-size: 22px">{{ activeTask.progress >= 0 ? activeTask.progress + '%' : '—' }}</span>
                <span class="faint tiny">{{ progressLabel }}</span>
              </div>
              <ProgressBar
                :value="activeTask.progress"
                :mode="activeTask.progressMode"
                :danger="activeTask.status === 'FAILED'"
                :label="t(activeTask.stageDetail || activeTask.stageLabel)"
                :meta="activeTask.processedBytes ? humanBytes(activeTask.processedBytes) + ' / ' + humanBytes(activeTask.totalBytes) : ''"
              />

              <div class="metric-row"><span class="metric-label">{{ t('当前阶段') }}</span><span class="metric-value">{{ t(activeTask.stageLabel || activeTask.stage) }}</span></div>
              <div class="metric-row"><span class="metric-label">{{ t('已用时间') }}</span><span class="metric-value">{{ formatDuration(activeTask.elapsedMs) }}</span></div>
              <div class="metric-row"><span class="metric-label">{{ t('目标量化') }}</span><span class="metric-value">{{ form.quantType || activeTask.params?.quantType || 'none' }}</span></div>
              <div class="metric-row"><span class="metric-label">{{ t('输入大小') }}</span><span class="metric-value">{{ humanBytes(activeTask.totalBytes || model?.totalSize) }}</span></div>

              <div class="status-rail" style="margin-top: 12px">
                <div
                  v-for="(stage, index) in stages"
                  :key="stage.id"
                  class="stage-item"
                  :class="{ done: stageIndex > index, active: stageIndex === index }"
                >
                  <span class="stage-dot"><Icon v-if="stageIndex > index" name="check" :size="8" :stroke="2" /><span v-else>{{ index + 1 }}</span></span>
                  <span>{{ stage.label }}</span>
                  <span v-if="stageIndex === index" class="faint tiny" style="margin-left: auto">{{ t('进行中') }}</span>
                </div>
              </div>

              <div v-if="activeTask.status === 'FAILED'" class="notice danger" style="margin-top: 12px">
                <Icon name="warning" :size="14" class="notice-icon" />
                <div>
                  <strong>{{ activeTask.errorCode }}</strong><br />{{ activeTask.errorMessage }}
                  <div v-if="activeTask.suggestion" class="faint tiny" style="margin-top: 5px">{{ activeTask.suggestion }}</div>
                  <button class="btn btn-sm" style="margin-top: 8px" @click="copyText(JSON.stringify(activeTask, null, 2))">{{ t('复制错误详情') }}</button>
                </div>
              </div>

              <div v-if="resultData?.ready" class="notice success" style="margin-top: 12px">
                <Icon name="check" :size="14" class="notice-icon" />
                <div style="min-width: 0">
                  <strong>{{ t('转换完成并通过结构验证') }}</strong><br />
                  <span class="mono tiny" style="word-break: break-all">{{ resultData.outputPath }}</span>
                  <div class="btn-row" style="margin-top: 8px">
                    <a class="btn btn-sm btn-primary" :href="`/api/v1/conversions/${activeTask.id}/download`"><Icon name="download" :size="12" /> {{ t('下载 GGUF') }}</a>
                    <a class="btn btn-sm" :href="`/api/v1/conversions/${activeTask.id}/report`"><Icon name="file" :size="12" /> {{ t('报告') }}</a>
                    <router-link class="btn btn-sm" to="/lab">{{ t('量化实验室') }}</router-link>
                  </div>
                </div>
              </div>

              <div class="section-label">{{ t('实时日志') }}</div>
              <LogConsole
                :entries="logs"
                :height="300"
                :title="activeTask.id + '.log'"
                :download-url="logUrl"
                @clear="logs = []"
              />
            </template>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
