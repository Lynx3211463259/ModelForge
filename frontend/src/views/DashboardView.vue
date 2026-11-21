<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, formatDuration, humanBytes, humanNumber, relativeTime } from '../api/client'
import type { ResourceInfo } from '../types'
import { useAppStore } from '../stores/app'
import { chartPalette, themeMode } from '../theme'
import Icon from '../components/Icon.vue'
import StatusBadge from '../components/StatusBadge.vue'
import ChartPanel from '../components/ChartPanel.vue'
import EmptyState from '../components/EmptyState.vue'
import ProgressBar from '../components/ProgressBar.vue'

const app = useAppStore()
const summary = ref<Record<string, any> | null>(null)
const resources = ref<ResourceInfo | null>(null)
const loading = ref(true)
const error = ref('')
const samples = ref<Array<{ label: string; cpu: number; mem: number }>>([])
let timer: number | undefined

const chartOption = computed(() => {
  const palette = themeMode.value ? chartPalette() : chartPalette()
  return {
  grid: { left: 34, right: 12, top: 26, bottom: 22 },
  tooltip: { trigger: 'axis', backgroundColor: palette.panel, borderColor: palette.lineStrong, textStyle: { color: palette.text, fontSize: 10 } },
  legend: { top: 0, right: 4, itemWidth: 8, itemHeight: 8, textStyle: { color: palette.muted, fontSize: 9 } },
  xAxis: {
    type: 'category',
    data: samples.value.map((item) => item.label),
    axisLine: { lineStyle: { color: palette.line } },
    axisTick: { show: false },
    axisLabel: { color: palette.faint, fontSize: 9 }
  },
  yAxis: {
    type: 'value',
    max: 100,
    splitLine: { lineStyle: { color: palette.lineSoft } },
    axisLabel: { color: palette.faint, fontSize: 9, formatter: '{value}%' }
  },
  series: [
    {
      name: 'CPU',
      type: 'line',
      data: samples.value.map((item) => item.cpu),
      showSymbol: false,
      smooth: true,
      lineStyle: { width: 1.4, color: palette.accent },
      areaStyle: { color: 'rgba(52,214,196,0.08)' }
    },
    {
      name: t('内存'),
      type: 'line',
      data: samples.value.map((item) => item.mem),
      showSymbol: false,
      smooth: true,
      lineStyle: { width: 1.4, color: palette.blue },
      areaStyle: { color: 'rgba(91,149,255,0.065)' }
    }
  ]
  }
})

const diskPercent = computed(() => {
  if (!resources.value?.diskTotal) return 0
  return ((resources.value.diskTotal - resources.value.diskFree) / resources.value.diskTotal) * 100
})

async function load() {
  try {
    const [data, resource] = await Promise.all([
      api.get<Record<string, any>>('/api/v1/system/summary'),
      api.get<ResourceInfo>('/api/v1/system/resources')
    ])
    summary.value = data
    resources.value = resource
    error.value = ''
    pushSample(resource)
  } catch (err) {
    error.value = err instanceof Error ? err.message : String(err)
  } finally {
    loading.value = false
  }
}

function pushSample(resource: ResourceInfo) {
  const now = new Date().toLocaleTimeString('zh-CN', { hour12: false }).slice(0, 5)
  samples.value = [...samples.value.slice(-17), { label: now, cpu: resource.cpuPercent, mem: resource.memoryPercent }]
}

async function pollResources() {
  try {
    resources.value = await api.get<ResourceInfo>('/api/v1/system/resources')
    pushSample(resources.value)
  } catch {
    // Keep previous sample when the backend is restarting.
  }
}

function taskTone(status: string) {
  return status === 'SUCCEEDED' ? 'success' : status === 'FAILED' ? 'danger' : status === 'RUNNING' ? 'active' : 'muted'
}

onMounted(() => {
  load()
  timer = window.setInterval(pollResources, 5000)
})
onUnmounted(() => { if (timer) window.clearInterval(timer) })
</script>

<template>
  <div>
    <section class="hero">
      <div class="hero-content">
        <div class="hero-eyebrow">Local model foundry / build 0.1.0</div>
        <h1 class="hero-title">{{ t('把权重变成') }}<br /><span>{{ t('可运行、可比较、可追溯') }}</span>{{ t('的模型产物。') }}</h1>
        <p class="hero-copy">
          {{ t('在本地完成 Safetensors 扫描、GGUF 转换、动态量化与结构验证。模型不离开你的磁盘，所有进度和日志都来自真实进程。') }}
        </p>
        <div class="hero-actions">
          <router-link to="/convert" class="btn btn-primary btn-lg">
            <Icon name="play" :size="14" /> {{ t('开始转换') }}
          </router-link>
          <router-link to="/models" class="btn btn-lg">
            <Icon name="plus" :size="14" /> {{ t('导入模型') }}
          </router-link>
          <router-link to="/analyzer" class="btn btn-lg btn-ghost">
            {{ t('分析模型结构') }} <Icon name="arrow" :size="13" />
          </router-link>
        </div>
      </div>
      <div class="hero-visual">
        <div class="hero-orbit">
          <div class="hero-core">
            <Icon name="layers" :size="34" :stroke="1.1" />
          </div>
        </div>
      </div>
    </section>

    <div v-if="!app.capabilities?.conversionReady" class="notice warning" style="margin-bottom: 14px">
      <Icon name="warning" :size="15" class="notice-icon" />
      <div>
        <strong>{{ t('转换引擎尚未完全就绪。') }}</strong>
        {{ t('应用可以启动、扫描模型与查看界面，但真实转换需要 Python 转换依赖与 llama-quantize。') }}
        <router-link to="/settings" style="margin-left: 5px">{{ t('前往设置检查 →') }}</router-link>
      </div>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在汇总本地工作区…') }}</div>
    <div v-else-if="error" class="notice danger"><Icon name="warning" :size="15" class="notice-icon" /> {{ error }}</div>

    <template v-else-if="summary">
      <div class="grid grid-4" style="margin-bottom: 14px">
        <div class="card stat-card stat-accent">
          <div class="stat-top"><span>{{ t('模型库') }}</span><span class="stat-icon"><Icon name="models" :size="14" /></span></div>
          <div>
            <div class="stat-value">{{ summary.models }}</div>
            <div class="stat-note">{{ t('{0} 已登记模型', [humanBytes(summary.totalModelSize)]) }}</div>
          </div>
        </div>
        <div class="card stat-card">
          <div class="stat-top"><span>{{ t('成功转换') }}</span><span class="stat-icon"><Icon name="check" :size="14" /></span></div>
          <div>
            <div class="stat-value">{{ summary.conversionsSucceeded }}</div>
            <div class="stat-note">{{ t('产物总计 {0}', [humanBytes(summary.convertedOutputSize)]) }}</div>
          </div>
        </div>
        <div class="card stat-card">
          <div class="stat-top"><span>{{ t('任务队列') }}</span><span class="stat-icon"><Icon name="tasks" :size="14" /></span></div>
          <div>
            <div class="stat-value">{{ (summary.queue.running || 0) + (summary.queue.queued || 0) }}</div>
            <div class="stat-note">{{ t('{0} 运行 · {1} 等待 · 并发 {2}', [summary.queue.running, summary.queue.queued, summary.queue.maxConcurrent]) }}</div>
          </div>
        </div>
        <div class="card stat-card">
          <div class="stat-top"><span>{{ t('输出磁盘') }}</span><span class="stat-icon"><Icon name="disk" :size="14" /></span></div>
          <div>
            <div class="stat-value" style="font-size: 20px">{{ humanBytes(resources?.diskFree) }}</div>
            <div class="stat-note">{{ t('可用 / {0} · 已用 {1}%', [humanBytes(resources?.diskTotal), diskPercent.toFixed(0)]) }}</div>
          </div>
        </div>
      </div>

      <div class="split-panel" style="margin-bottom: 14px">
        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">{{ t('系统资源') }}</div>
              <div class="card-subtitle">{{ t('每 5 秒采样 · 真实本机指标') }}</div>
            </div>
            <div class="card-header-actions">
              <span class="tag">{{ resources?.os }}</span>
              <span class="tag">{{ resources?.host }}</span>
            </div>
          </div>
          <div class="card-body">
            <div class="grid grid-3" style="margin-bottom: 8px">
              <div>
                <div class="field-label">CPU <span class="mono">{{ resources?.cpuPercent ?? 0 }}%</span></div>
                <ProgressBar :value="resources?.cpuPercent ?? 0" />
              </div>
              <div>
                <div class="field-label">{{ t('内存') }} <span class="mono">{{ resources?.memoryPercent ?? 0 }}%</span></div>
                <ProgressBar :value="resources?.memoryPercent ?? 0" />
              </div>
              <div>
                <div class="field-label">{{ t('输出磁盘') }} <span class="mono">{{ diskPercent.toFixed(0) }}%</span></div>
                <ProgressBar :value="diskPercent" />
              </div>
            </div>
            <ChartPanel :option="chartOption" :height="210" />
            <div class="well" style="margin-top: 4px">
              <div class="well-row"><span>GPU</span><span class="mono">{{ t(resources?.gpu || '—') }}</span></div>
              <div class="well-row"><span>{{ t('内存可用') }}</span><span class="mono">{{ humanBytes(resources?.memoryAvailable) }}</span></div>
              <div class="well-row"><span>{{ t('默认线程') }}</span><span class="mono">{{ resources?.threads }}</span></div>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div>
              <div class="card-title">{{ t('最近任务') }}</div>
              <div class="card-subtitle">{{ t('转换状态与真实进度') }}</div>
            </div>
            <div class="card-header-actions"><router-link class="btn btn-sm btn-ghost" to="/tasks">{{ t('全部任务') }} <Icon name="arrow" :size="12" /></router-link></div>
          </div>
          <div v-if="!summary.recentTasks?.length" class="empty-state" style="padding: 32px">
            <div class="empty-title">{{ t('还没有转换任务') }}</div>
            <div class="empty-copy">{{ t('选择一个完整模型目录，转换后会在这里显示阶段、进度与耗时。') }}</div>
            <router-link to="/convert" class="btn btn-sm btn-primary">{{ t('创建第一个任务') }}</router-link>
          </div>
          <div v-else class="card-body flush table-wrap">
            <table class="table">
              <thead><tr><th>{{ t('模型 / 任务') }}</th><th>{{ t('状态') }}</th><th>{{ t('进度') }}</th><th>{{ t('耗时') }}</th><th></th></tr></thead>
              <tbody>
                <tr v-for="task in summary.recentTasks.slice(0, 5)" :key="task.id">
                  <td>
                    <div class="strong truncate" style="max-width: 190px">{{ task.modelName }}</div>
                    <div class="faint mono tiny">{{ task.id }}</div>
                  </td>
                  <td><span class="badge" :class="taskTone(task.status)">{{ t(task.stageLabel || task.status) }}</span></td>
                  <td style="min-width: 100px">
                    <ProgressBar :value="task.progress" :mode="task.progressMode" />
                  </td>
                  <td class="mono small">{{ formatDuration(task.elapsedMs) }}</td>
                  <td class="mono tiny faint">{{ relativeTime(task.createdAt) }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <div class="grid grid-2">
        <div class="card">
          <div class="card-header">
            <div><div class="card-title">{{ t('最近导入') }}</div><div class="card-subtitle">{{ t('模型库中最近更新的条目') }}</div></div>
            <div class="card-header-actions"><router-link class="btn btn-sm btn-ghost" to="/models">{{ t('打开模型库') }}</router-link></div>
          </div>
          <div v-if="!summary.recentModels?.length" class="empty-state" style="padding: 30px">
            <div class="empty-title">{{ t('模型库为空') }}</div>
            <router-link to="/models" class="btn btn-sm" style="margin-top: 12px">{{ t('导入本地模型') }}</router-link>
          </div>
          <div v-else class="card-body">
            <div v-for="model in summary.recentModels" :key="model.id" style="display: flex; align-items: center; gap: 11px; padding: 10px 0; border-bottom: 1px solid var(--line-soft)">
              <div class="model-avatar">{{ (model.architectureFamily || 'AI').slice(0, 2).toUpperCase() }}</div>
              <div style="min-width: 0; flex: 1">
                <div class="strong truncate">{{ model.name }}</div>
                <div class="faint tiny truncate">{{ model.architecture }} · {{ model.dtype || 'unknown dtype' }}</div>
              </div>
              <div class="mono tiny muted">{{ humanNumber(model.parameterCount) }}</div>
              <StatusBadge :status="model.status" />
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><div><div class="card-title">{{ t('引擎与依赖') }}</div><div class="card-subtitle">{{ t('当前 llama.cpp 工具链状态') }}</div></div>
            <div class="card-header-actions"><button class="btn btn-sm" @click="app.refreshCapabilities(true)"><Icon name="refresh" :size="12" /> {{ t('重新检测') }}</button></div>
          </div>
          <div class="card-body">
            <div class="engine-chip" style="margin-bottom: 7px">
              <span class="status-dot" :class="app.capabilities?.convertScriptAvailable ? 'online' : 'offline'"></span>
              convert_hf_to_gguf.py
              <span class="meta">{{ app.capabilities?.convertScriptAvailable ? 'FOUND' : 'MISSING' }}</span>
            </div>
            <div class="engine-chip" style="margin-bottom: 7px">
              <span class="status-dot" :class="app.capabilities?.pythonDependenciesReady ? 'online' : 'offline'"></span>
              Python / torch / transformers
              <span class="meta">{{ app.capabilities?.pythonDependenciesReady ? 'READY' : 'BLOCKED' }}</span>
            </div>
            <div class="engine-chip" style="margin-bottom: 7px">
              <span class="status-dot" :class="app.capabilities?.quantizeAvailable ? 'online' : 'offline'"></span>
              llama-quantize
              <span class="meta">{{ app.capabilities?.quantTypes?.length || 0 }} TYPES</span>
            </div>
            <div class="engine-chip">
              <span class="status-dot" :class="app.capabilities?.cliAvailable ? 'online' : 'offline'"></span>
              llama-cli / tester
              <span class="meta">{{ app.capabilities?.cliAvailable ? 'READY' : 'UNAVAILABLE' }}</span>
            </div>
            <div v-if="app.capabilities?.issues?.length" class="notice warning" style="margin-top: 11px">
              <Icon name="info" :size="14" class="notice-icon" />
              <div>{{ app.capabilities.issues[0] }}</div>
            </div>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>
