<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, formatDuration, humanBytes, humanNumber } from '../api/client'
import type { EstimateOption, Model, Task } from '../types'
import { useAppStore } from '../stores/app'
import { chartPalette, themeMode } from '../theme'
import Icon from '../components/Icon.vue'
import ChartPanel from '../components/ChartPanel.vue'
import EmptyState from '../components/EmptyState.vue'
import StatusBadge from '../components/StatusBadge.vue'
import ProgressBar from '../components/ProgressBar.vue'

const app = useAppStore()
const models = ref<Model[]>([])
const modelId = ref('')
const estimates = ref<EstimateOption[]>([])
const selectedQuants = ref<string[]>([])
const launching = ref(false)
const tasks = ref<Task[]>([])
const benchmarks = ref<any[]>([])
const benchmarkingId = ref('')
const compare = ref<any>(null)
const pplJob = ref<any>(null)
const pplContext = ref(512)
const pplDatasetPath = ref('')
const pplBusy = ref(false)
let pplTimer: number | undefined

const availableOptions = computed(() => estimates.value.filter((item) => item.available && !['F16', 'BF16', 'F32'].includes(item.type.toUpperCase())))
const selectedModel = computed(() => models.value.find((item) => item.id === modelId.value) || null)

const sizeChart = computed(() => {
  const palette = themeMode.value ? chartPalette() : chartPalette()
  const options = availableOptions.value
  return {
    grid: { left: 54, right: 16, top: 26, bottom: 36 },
    tooltip: { trigger: 'axis', backgroundColor: palette.panel, borderColor: palette.lineStrong, textStyle: { color: palette.text, fontSize: 10 }, valueFormatter: (value: number) => humanBytes(value) },
    xAxis: { type: 'category', data: options.map((item) => item.type), axisLine: { lineStyle: { color: palette.line } }, axisTick: { show: false }, axisLabel: { color: palette.muted, fontSize: 9 } },
    yAxis: { type: 'value', splitLine: { lineStyle: { color: palette.lineSoft } }, axisLabel: { color: palette.faint, fontSize: 9, formatter: (value: number) => humanBytes(value) } },
    series: [{
      type: 'bar',
      barMaxWidth: 24,
      data: options.map((item) => ({
        value: item.estimatedSize,
        itemStyle: {
          color: selectedQuants.value.includes(item.type) ? '#34d6c4' : '#2b638f',
          borderRadius: [4, 4, 0, 0]
        }
      }))
    }]
  }
})

const memoryChart = computed(() => {
  const palette = themeMode.value ? chartPalette() : chartPalette()
  const options = availableOptions.value
  return {
    grid: { left: 54, right: 16, top: 24, bottom: 36 },
    tooltip: { trigger: 'axis', backgroundColor: palette.panel, borderColor: palette.lineStrong, textStyle: { color: palette.text, fontSize: 10 }, valueFormatter: (value: number) => humanBytes(value) },
    xAxis: { type: 'category', data: options.map((item) => item.type), axisLine: { lineStyle: { color: palette.line } }, axisTick: { show: false }, axisLabel: { color: palette.muted, fontSize: 9 } },
    yAxis: { type: 'value', splitLine: { lineStyle: { color: palette.lineSoft } }, axisLabel: { color: palette.faint, fontSize: 9, formatter: (value: number) => humanBytes(value) } },
    series: [{
      type: 'line',
      smooth: true,
      showSymbol: true,
      symbolSize: 5,
      data: options.map((item) => item.estimatedRuntimeMemory),
      lineStyle: { color: '#5b95ff', width: 1.4 },
      itemStyle: { color: '#5b95ff' },
      areaStyle: { color: 'rgba(91,149,255,0.07)' }
    }]
  }
})

const modelTasks = computed(() => tasks.value.filter((task) => !modelId.value || task.modelId === modelId.value))
const comparison = computed(() => modelTasks.value.filter((task) => Boolean(task.params?.quantType)))

async function loadModels() {
  try {
    models.value = await api.get<Model[]>('/api/v1/models?limit=500')
    if (models.value.length) {
      modelId.value = models.value[0].id
      await selectModel(modelId.value)
    }
  } catch (error) {
    app.fromError(error, t('加载实验室数据失败'))
  }
}

async function selectModel(id: string) {
  if (!id) return
  try {
    const result = await api.get<{ options: EstimateOption[] }>(`/api/v1/models/${id}/estimates`)
    estimates.value = result.options || []
    const candidates = availableOptions.value.map((item) => item.type)
    selectedQuants.value = ['Q4_K_M', 'Q5_K_M', 'Q6_K'].filter((item) => candidates.includes(item))
    if (!selectedQuants.value.length) selectedQuants.value = candidates.slice(0, 2)
    await Promise.all([loadTasks(), loadBenchmarks(), loadComparison()])
  } catch (error) {
    app.fromError(error, t('读取量化估算失败'))
  }
}

async function loadTasks() {
  try {
    tasks.value = await api.get<Task[]>('/api/v1/conversions?limit=300')
  } catch {
    tasks.value = []
  }
}

async function loadBenchmarks() {
  try {
    benchmarks.value = await api.get<any[]>('/api/v1/benchmarks?limit=100')
  } catch {
    benchmarks.value = []
  }
}

function toggleQuant(type: string) {
  if (selectedQuants.value.includes(type)) selectedQuants.value = selectedQuants.value.filter((item) => item !== type)
  else selectedQuants.value = [...selectedQuants.value, type]
}

async function launchBatch() {
  if (!selectedModel.value || !selectedQuants.value.length) return
  if (!app.capabilities?.conversionReady) {
    app.toast('warning', t('转换链路尚未就绪'), t('请先在设置中检查工具路径与 Python 依赖'))
    return
  }
  launching.value = true
  const created: Task[] = []
  const failed: string[] = []
  try {
    for (const quant of selectedQuants.value) {
      try {
        const task = await api.post<Task>('/api/v1/conversions', {
          modelId: selectedModel.value.id,
          inputPath: selectedModel.value.path,
          outputDir: app.settings?.defaultOutputDir,
          outputName: `${selectedModel.value.name}.${quant}`,
          outType: 'f16',
          quantType: quant,
          threads: app.settings?.threads,
          verify: true,
          computeSha256: true,
          overwrite: false
        })
        created.push(task)
      } catch (error) {
        failed.push(`${quant}: ${error instanceof Error ? error.message : String(error)}`)
      }
    }
    if (created.length) {
      app.toast('success', t('已创建 {0} 个量化任务', [created.length]), t('任务将在队列中按并发限制执行'))
      await loadTasks()
    }
    if (failed.length) app.toast('warning', t('{0} 个方案未能入队', [failed.length]), failed[0])
  } finally {
    launching.value = false
  }
}

async function loadComparison() {
  if (!modelId.value) return
  try {
    compare.value = await api.get<any>(`/api/v1/quality/compare?modelId=${encodeURIComponent(modelId.value)}`)
  } catch {
    compare.value = null
  }
}

async function loadPplStatus() {
  try {
    const status = await api.get<any>('/api/v1/quality/ppl/status')
    pplJob.value = status
    if (status.status === 'running') {
      pplBusy.value = true
      if (!pplTimer) {
        pplTimer = window.setInterval(async () => {
          const next = await api.get<any>('/api/v1/quality/ppl/status')
          pplJob.value = next
          if (next.status !== 'running') {
            if (pplTimer) { window.clearInterval(pplTimer); pplTimer = undefined }
            pplBusy.value = false
            if (next.status === 'completed') app.toast('success', t('质量评测完成'), next.message)
            else if (next.status === 'failed') app.toast('error', t('质量评测失败'), next.errorMessage || '')
            await loadComparison()
          }
        }, 2000)
      }
    }
  } catch {
    // status is optional; ignore transient failures
  }
}

async function runPpl() {
  if (!compare.value?.rows?.length) return
  const targets = compare.value.rows
    .filter((row: any) => row.exists)
    .map((row: any) => ({ path: row.path, versionId: row.versionId, quantType: row.quantType }))
  if (!targets.length) {
    app.toast('warning', t('没有可评测的 GGUF 文件'), t('请先完成转换并确认产物仍在磁盘上'))
    return
  }
  pplBusy.value = true
  try {
    pplJob.value = await api.post<any>('/api/v1/quality/ppl/run', {
      modelId: modelId.value,
      targets,
      context: Number(pplContext.value),
      datasetPath: pplDatasetPath.value.trim()
    })
    app.toast('success', t('质量评测已启动'), t('{0} 个文件', [targets.length]))
    await loadPplStatus()
  } catch (error) {
    pplBusy.value = false
    app.fromError(error, t('质量评测失败'))
  }
}

async function cancelPpl() {
  try {
    pplJob.value = await api.post<any>('/api/v1/quality/ppl/cancel')
  } catch (error) {
    app.fromError(error, t('取消失败'))
  }
}

function pplFor(row: any) {
  return row.ppl && typeof row.ppl.ppl === 'number' && row.ppl.ppl > 0 ? row.ppl : null
}

async function runBenchmark(task: Task) {
  if (!task.outputPath) return
  benchmarkingId.value = task.id
  try {
    await api.post('/api/v1/benchmarks/run', { path: task.outputPath, modelId: task.modelId, quantType: task.params?.quantType || "" })
    app.toast('success', t('基准测试完成'), t('结果来自 llama-bench 实际运行'))
    await loadBenchmarks()
  } catch (error) {
    app.fromError(error, t('基准测试失败'))
  } finally {
    benchmarkingId.value = ''
  }
}

function benchmarkFor(task: Task) {
  const target = (task.outputPath || '').replace(/\\/g, '/').toLowerCase()
  if (!target) return undefined
  return benchmarks.value.find((item) => {
    const filename = String(item.metrics?.[0]?.model_filename || '').replace(/\\/g, '/').toLowerCase()
    return Boolean(filename) && filename === target
  })
}

function avgTokensPerSecond(metrics: any[]): string {
  if (!Array.isArray(metrics) || !metrics.length) return '—'
  const generation = metrics.find((item) => Number(item.n_gen) > 0)
  const prompt = metrics.find((item) => Number(item.n_prompt) > 0)
  const value = Number((generation || prompt)?.avg_ts || (generation || prompt)?.ts || 0)
  return value > 0 ? value.toFixed(2) + (generation ? '' : '*') : '—'
}

onMounted(async () => {
  await loadModels()
  await loadPplStatus()
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('量化实验室') }}</h1>
        <p class="page-subtitle">{{ t('同一模型生成多个真实量化产物，比较体积、耗时与实测基准。不会生成伪造的质量分数。') }}</p>
      </div>
      <div class="page-actions">
        <select v-if="models.length" v-model="modelId" class="select" style="width: 260px" @change="selectModel(modelId)">
          <option v-for="item in models" :key="item.id" :value="item.id">{{ item.name }}</option>
        </select>
        <button class="btn" @click="loadTasks"><Icon name="refresh" :size="13" /> {{ t('刷新结果') }}</button>
      </div>
    </div>

    <div v-if="!models.length" class="card">
      <EmptyState icon="lab" :title="t('实验室需要至少一个模型')" copy="先导入模型，再选择多个量化方案批量生成真实 GGUF 产物。">
        <router-link to="/models" class="btn btn-primary">{{ t('导入模型') }}</router-link>
      </EmptyState>
    </div>

    <template v-else>
      <div class="split-panel" style="margin-bottom: 14px">
        <div class="card">
          <div class="card-header"><div><div class="card-title">{{ t('选择量化方案') }}</div><div class="card-subtitle">{{ t('仅展示当前 llama-quantize 实际支持的类型') }}</div></div><div class="card-header-actions"><span class="tag">{{ selectedQuants.length }} SELECTED</span></div></div>
          <div class="card-body">
            <div v-if="!availableOptions.length" class="notice warning"><Icon name="warning" :size="14" class="notice-icon" />{{ t('未检测到可用量化类型。请先完成 llama-quantize 配置。') }}</div>
            <div class="option-list" v-else>
              <label v-for="option in availableOptions" :key="option.type" class="option-card" :class="{ selected: selectedQuants.includes(option.type) }">
                <input type="checkbox" :checked="selectedQuants.includes(option.type)" style="display: none" @change="toggleQuant(option.type)" />
                <span class="option-radio"></span>
                <div><div class="option-name">{{ option.type }}</div><div class="option-desc">{{ t(option.description) }}</div></div>
                <div class="option-size">{{ humanBytes(option.estimatedSize) }}<br /><span class="faint">{{ option.bitsPerWeight }} bpw</span></div>
              </label>
            </div>
            <div class="btn-row" style="margin-top: 14px">
              <button class="btn btn-primary" :disabled="launching || !selectedQuants.length || !app.capabilities?.conversionReady" @click="launchBatch">
                <span v-if="launching" class="spinner"></span>
                <Icon v-else name="play" :size="13" /> {{ t('{0}', [launching ? t('创建任务…') : t('生成所选量化版本')]) }}</button>
              <span class="faint tiny">{{ t('每个方案会独立完成转换与量化，便于单独追溯。') }}</span>
            </div>
            <div v-if="selectedModel" class="notice info" style="margin-top: 12px">
              <Icon name="info" :size="14" class="notice-icon" />
              <div>{{ t('{0} · {1} 参数 · {2}。预计将占用约 {3} 输出空间。', [selectedModel.name, humanNumber(selectedModel.parameterCount), humanBytes(selectedModel.totalSize), humanBytes(selectedQuants.reduce((sum, q) => sum + (estimates.find(e => e.type === q)?.estimatedSize || 0), 0))]) }}</div>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><div><div class="card-title">{{ t('体积与运行内存预测') }}</div><div class="card-subtitle">{{ t('参数规模 × 每权重比特数；内存为启发式估算') }}</div></div></div>
          <div class="card-body">
            <ChartPanel :option="sizeChart" :height="210" />
            <ChartPanel :option="memoryChart" :height="170" />
            <div class="faint tiny">{{ t('预测值用于规划磁盘与内存，不等同于 llama.cpp 的实际加载占用。实测请运行基准测试。') }}</div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header"><div><div class="card-title">{{ t('实验结果') }}</div><div class="card-subtitle">{{ t('该模型的真实量化任务与 llama-bench 测量') }}</div></div><div class="card-header-actions"><span class="tag">{{ comparison.length }} VARIANTS</span></div></div>
        <div v-if="!comparison.length" class="empty-state" style="padding: 38px">
          <div class="empty-title">{{ t('还没有量化结果') }}</div>
          <div class="empty-copy">{{ t('选择方案并创建任务后，文件体积、转换耗时和验证状态会随任务更新。') }}</div>
        </div>
        <div v-else class="table-wrap">
          <table class="table">
            <thead><tr><th>{{ t('量化') }}</th><th>{{ t('状态') }}</th><th>{{ t('文件大小') }}</th><th>{{ t('转换耗时') }}</th><th>{{ t('Gen Tok/s (实测)') }}</th><th>{{ t('验证') }}</th><th>{{ t('产物') }}</th><th></th></tr></thead>
            <tbody>
              <tr v-for="task in comparison" :key="task.id">
                <td><span class="tag">{{ task.params?.quantType }}</span><div class="faint mono tiny">{{ task.id }}</div></td>
                <td><StatusBadge :status="task.status" :label="task.stageLabel" /></td>
                <td class="mono small">{{ humanBytes(task.outputSize) || '—' }}</td>
                <td class="mono small">{{ formatDuration(task.elapsedMs) }}</td>
                <td class="mono small">{{ avgTokensPerSecond(benchmarkFor(task)?.metrics) }}</td>
                <td><span class="badge no-dot" :class="task.verification?.structurallyValid ? 'success' : 'muted'">{{ t('{0}', [task.verification?.structurallyValid ? t('结构通过') : t('待验证')]) }}</span></td>
                <td class="mono tiny truncate" style="max-width: 200px" :title="task.outputPath">{{ task.outputPath || '—' }}</td>
                <td>
                  <div class="table-actions">
                    <a v-if="task.status === 'SUCCEEDED'" class="icon-btn" :title="t('下载')" :href="`/api/v1/conversions/${task.id}/download`"><Icon name="download" :size="12" /></a>
                    <button v-if="task.status === 'SUCCEEDED'" class="btn btn-sm" :disabled="benchmarkingId === task.id || !app.capabilities?.benchAvailable" :title="!app.capabilities?.benchAvailable ? t('llama-bench 不可用') : t('运行 llama-bench')" @click="runBenchmark(task)">
                      <span v-if="benchmarkingId === task.id" class="spinner" style="width: 10px; height: 10px"></span>
                      <Icon v-else name="activity" :size="11" /> {{ t('基准') }}
                    </button>
                    <button v-if="['FAILED','CANCELED'].includes(task.status)" class="icon-btn" :title="t('重试')" @click="api.post(`/api/v1/conversions/${task.id}/retry`).then(loadTasks)"><Icon name="retry" :size="12" /></button>
                  </div>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <div class="card-body" style="border-top: 1px solid var(--line-soft)">
          <div class="notice">
            <Icon name="info" :size="14" class="notice-icon" />
            <div>{{ t('困惑度评测在下方卡片中运行；不同评测文本或上下文长度的结果不可直接比较。') }}</div>
          </div>
        </div>
      </div>
    </template>

    <div class="card" style="margin-top: 14px">
      <div class="card-header">
        <div>
          <div class="card-title">{{ t('困惑度质量评测') }}</div>
          <div class="card-subtitle">{{ t('用 llama-perplexity 在固定文本上测量每个产物的困惑度；数值越低代表模型对文本越确定。') }}</div>
        </div>
        <div class="card-header-actions">
          <span class="tag">{{ (compare?.rows || []).filter((row: any) => row.exists).length }} GGUF</span>
        </div>
      </div>
      <div class="card-body" style="border-bottom: 1px solid var(--line-soft)">
        <div class="field-row" style="grid-template-columns: 150px minmax(0, 1fr) 190px">
          <div class="field">
            <label class="field-label">{{ t('上下文长度') }}</label>
            <input v-model.number="pplContext" class="input mono" type="number" min="128" max="4096" step="128" />
          </div>
          <div class="field" style="flex: 1">
            <label class="field-label">{{ t('评测文本（留空使用默认）') }}</label>
            <input v-model="pplDatasetPath" class="input mono" :placeholder="pplJob?.dataset?.path || t('默认内置示例文本')" />
          </div>
          <div class="field" style="justify-content: flex-end">
            <button v-if="pplBusy" class="btn" @click="cancelPpl"><Icon name="close" :size="13" /> {{ t('取消') }}</button>
            <button v-else class="btn btn-primary" :disabled="!app.capabilities?.pplAvailable || !(compare?.rows || []).length" @click="runPpl">
              <Icon name="activity" :size="13" /> {{ t('运行 PPL 评测') }}
            </button>
          </div>
        </div>
        <div class="notice" :class="pplJob?.dataset?.standard ? 'info' : 'warning'" style="margin-top: 10px">
          <Icon name="info" :size="14" class="notice-icon" />
          <div>
            <div v-if="!app.capabilities?.pplAvailable">{{ t('未检测到 llama-perplexity，请在“运行引擎”页安装官方 llama.cpp。') }}</div>
            <div v-else-if="pplJob?.dataset">
              {{ pplJob.dataset.kind === 'builtin-sample'
                ? t('内置示例文本不是标准测试集，只用于同一模型不同量化之间的相对比较；如需与公开数值对比，请把 wiki.test.raw 放到 data/datasets/。')
                : t('评测文本：{0}', [pplJob.dataset.path]) }}
              <span class="faint"> · {{ humanBytes(pplJob.dataset.bytes || 0) }}</span>
            </div>
          </div>
        </div>
        <div v-if="pplBusy && pplJob" style="margin-top: 10px">
          <ProgressBar :value="pplJob.progress" :mode="'measured'" />
          <div class="faint tiny" style="margin-top: 6px">{{ pplJob.message }} · {{ pplJob.completed }}/{{ pplJob.total }}</div>
        </div>
      </div>
      <div v-if="!compare?.rows?.length" class="empty-state" style="padding: 32px">
        <div class="empty-title">{{ t('还没有可评测的产物') }}</div>
        <div class="empty-copy">{{ t('先完成一次转换，版本树中的每个 GGUF 都会出现在这里。') }}</div>
      </div>
      <div v-else class="table-wrap">
        <table class="table">
          <thead><tr><th>{{ t('量化') }}</th><th>{{ t('体积') }}</th><th>PPL</th><th>Δ</th><th>{{ t('Token 数') }}</th><th>{{ t('耗时') }}</th><th>{{ t('峰值内存') }}</th><th>Gen Tok/s</th></tr></thead>
          <tbody>
            <tr v-for="row in compare.rows" :key="row.versionId">
              <td><span class="tag">{{ row.quantType || 'GGUF' }}</span></td>
              <td class="mono small">{{ humanBytes(row.size) }}</td>
              <td class="mono small">{{ pplFor(row) ? pplFor(row).ppl.toFixed(4) : '—' }}</td>
              <td class="mono small" :style="row.pplDelta > 0 ? 'color: var(--amber)' : ''">{{ row.pplDelta !== undefined ? (row.pplDelta > 0 ? '+' : '') + row.pplDelta.toFixed(2) + '%' : '—' }}</td>
              <td class="mono small">{{ pplFor(row) ? pplFor(row).tokens : '—' }}</td>
              <td class="mono small">{{ pplFor(row) ? formatDuration(pplFor(row).durationMs) : '—' }}</td>
              <td class="mono small">{{ pplFor(row) ? humanBytes(pplFor(row).peakMemory) : '—' }}</td>
              <td class="mono small">{{ row.benchmark?.genTps ? row.benchmark.genTps.toFixed(2) : '—' }}</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div class="card-body" style="border-top: 1px solid var(--line-soft)">
        <div class="faint tiny">{{ t('困惑度只反映模型对评测文本的确定性，不等于任务质量；不同文本、不同上下文的结果不可直接比较。') }}</div>
        <div v-if="pplJob?.logs?.length" class="mono tiny" style="margin-top: 8px; max-height: 120px; overflow: auto">
          <div v-for="(line, index) in pplJob.logs.slice(-6)" :key="index" class="faint">{{ line.message }}</div>
        </div>
      </div>
    </div>
  </div>
</template>
