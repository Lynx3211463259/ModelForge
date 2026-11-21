<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, humanBytes } from '../api/client'
import type { Task } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import EmptyState from '../components/EmptyState.vue'

const app = useAppStore()
const completed = ref<Task[]>([])
const selectedTaskId = ref('')
const manualPath = ref('')
const running = ref(false)
const result = ref<Record<string, any> | null>(null)
const form = ref({
  prompt: t('用三句话解释本地 AI 模型量化的意义。'),
  temperature: 0.7,
  topP: 0.95,
  topK: 40,
  maxTokens: 160,
  context: 2048,
  gpuLayers: 0,
  threads: 4
})

const selectedTask = computed(() => completed.value.find((item) => item.id === selectedTaskId.value) || null)
const path = computed(() => manualPath.value.trim() || selectedTask.value?.outputPath || '')

async function load() {
  try {
    const [tasks, settings] = await Promise.all([
      api.get<Task[]>('/api/v1/conversions?status=SUCCEEDED&limit=200'),
      api.get<any>('/api/v1/settings')
    ])
    completed.value = tasks
    form.value.threads = settings.threads || 4
    if (tasks.length) selectedTaskId.value = tasks[0].id
  } catch (error) {
    app.fromError(error, t('加载测试模型失败'))
  }
}

async function run() {
  if (!path.value) {
    app.toast('warning', t('请选择已完成的 GGUF 或输入路径'))
    return
  }
  running.value = true
  result.value = null
  try {
    result.value = await api.post('/api/v1/tester/run', { path: path.value, ...form.value })
    if (result.value?.exitCode !== 0) app.toast('warning', t('推理进程返回非零退出码'), t('退出码 {0}', [result.value?.exitCode]))
  } catch (error) {
    app.fromError(error, t('模型测试失败'))
  } finally {
    running.value = false
  }
}

onMounted(load)
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('模型运行测试器') }}</h1>
        <p class="page-subtitle">{{ t('直接调用 llama-cli 做真实、一次性的本地推理。输出包含工具运行日志，不做在线代理。') }}</p>
      </div>
      <div class="page-actions">
        <span class="badge" :class="app.capabilities?.cliAvailable ? 'success' : 'danger'">{{ t('{0}', [app.capabilities?.cliAvailable ? t('llama-cli 可用') : t('llama-cli 不可用')]) }}</span>
        <router-link to="/settings" class="btn">{{ t('检查工具路径') }}</router-link>
      </div>
    </div>

    <div v-if="!app.capabilities?.cliAvailable" class="notice warning" style="margin-bottom: 14px">
      <Icon name="warning" :size="15" class="notice-icon" />
      <div>{{ t('需要 llama.cpp 的') }} <span class="mono">llama-cli</span> {{ t('可执行文件。ModelForge 不会用模拟推理替代真实进程。') }}</div>
    </div>

    <div class="split-panel">
      <div class="card">
        <div class="card-header"><div><div class="card-title">{{ t('选择 GGUF') }}</div><div class="card-subtitle">{{ t('来自已完成任务或本地路径') }}</div></div></div>
        <div class="card-body">
          <div v-if="completed.length" class="field">
            <label class="field-label">{{ t('已完成转换') }}</label>
            <select v-model="selectedTaskId" class="select">
              <option v-for="task in completed" :key="task.id" :value="task.id">{{ task.modelName }} · {{ task.params?.quantType || task.params?.outType }} · {{ humanBytes(task.outputSize) }}</option>
            </select>
          </div>
          <div class="field">
            <label class="field-label">{{ t('或输入 GGUF 绝对路径') }}</label>
            <input v-model="manualPath" class="input mono" placeholder="C:\output\model.Q4_K_M.gguf" />
            <div class="field-hint">{{ t('手动路径优先于上方选择。') }}</div>
          </div>

          <div class="section-label">{{ t('生成参数') }}</div>
          <div class="field">
            <label class="field-label">{{ t('提示词') }}</label>
            <textarea v-model="form.prompt" class="textarea" style="min-height: 110px" />
          </div>
          <div class="field-row">
            <div class="field"><label class="field-label">Temperature <span class="mono">{{ form.temperature }}</span></label><input v-model.number="form.temperature" type="range" min="0" max="2" step="0.05" style="width: 100%" /></div>
            <div class="field"><label class="field-label">Top P <span class="mono">{{ form.topP }}</span></label><input v-model.number="form.topP" type="range" min="0.1" max="1" step="0.01" style="width: 100%" /></div>
          </div>
          <div class="field-row">
            <div class="field"><label class="field-label">Top K</label><input v-model.number="form.topK" type="number" min="0" max="1000" class="input" /></div>
            <div class="field"><label class="field-label">{{ t('最大生成 Token') }}</label><input v-model.number="form.maxTokens" type="number" min="1" max="4096" class="input" /></div>
          </div>
          <div class="field-row">
            <div class="field"><label class="field-label">{{ t('上下文长度') }}</label><input v-model.number="form.context" type="number" min="128" max="131072" class="input" /></div>
            <div class="field"><label class="field-label">GPU Layers</label><input v-model.number="form.gpuLayers" type="number" min="0" max="999" class="input" /></div>
          </div>
          <div class="field">
            <label class="field-label">{{ t('CPU 线程') }}</label>
            <input v-model.number="form.threads" type="number" min="1" max="512" class="input" />
          </div>

          <button class="btn btn-primary btn-lg" style="width: 100%" :disabled="running || !app.capabilities?.cliAvailable || !path" @click="run">
            <span v-if="running" class="spinner"></span><Icon v-else name="play" :size="14" /> {{ t('{0}', [running ? t('模型加载与推理中…') : t('运行真实推理')]) }}</button>
          <div class="faint tiny" style="margin-top: 8px">{{ t('首次加载大型 GGUF 可能需要较长时间；请求超时为 5 分钟。') }}</div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div><div class="card-title">{{ t('输出') }}</div><div class="card-subtitle">{{ t('llama-cli 原始标准输出与运行日志') }}</div></div>
          <div class="card-header-actions"><span v-if="result" class="tag">{{ (result.elapsedMs / 1000).toFixed(1) }}s · exit {{ result.exitCode }}</span></div>
        </div>
        <div class="card-body">
          <div v-if="!result && !running" class="empty-state" style="padding: 70px 20px">
            <div class="empty-icon"><Icon name="tester" :size="22" /></div>
            <div class="empty-title">{{ t('等待推理') }}</div>
            <div class="empty-copy">{{ t('选择产物并输入提示词。所有结果来自本机 llama-cli，不会上传提示词。') }}</div>
          </div>
          <div v-else-if="running" class="loading-panel" style="min-height: 300px; flex-direction: column">
            <span class="spinner"></span>
            <div>{{ t('正在加载模型并生成…') }}</div>
            <div class="faint tiny">{{ t('大型模型加载可能占用大量内存，请勿重复提交。') }}</div>
          </div>
          <template v-else-if="result">
            <div v-if="!result.started || result.exitCode !== 0" class="notice danger" style="margin-bottom: 10px">
              <Icon name="warning" :size="14" class="notice-icon" />
              <div>{{ t('llama-cli 未能正常完成。退出码 {0}。请查看下方原始输出与设置中的工具路径。', [result.exitCode]) }}</div>
            </div>
            <div class="kv" style="margin-bottom: 8px"><span>{{ t('实际路径') }}</span><span>{{ path }}</span></div>
            <pre class="code-block" style="max-height: 540px; overflow: auto; margin: 0">{{ t('{0}', [result.output || t('（进程没有标准输出）')]) }}</pre>
          </template>
        </div>
      </div>
    </div>
  </div>
</template>
