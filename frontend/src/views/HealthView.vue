<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, humanBytes, formatTime } from '../api/client'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'

interface CheckItem {
  id: string
  category: string
  title: string
  status: 'ok' | 'warn' | 'error'
  detail: string
  hint: string
  fix: string
  fixable: boolean
}

interface Report {
  status: 'ok' | 'warn' | 'error'
  checkedAt: string
  elapsedMs: number
  deep: boolean
  networkChecked: boolean
  summary: { total: number; ok: number; warn: number; error: number }
  items: CheckItem[]
  environment?: Record<string, any>
}

const app = useAppStore()

const report = ref<Report | null>(null)
const loading = ref(false)
const busy = ref('')
const deep = ref(false)
const network = ref(false)
const filter = ref<'all' | 'warn' | 'error'>('all')

const categoryLabels: Record<string, string> = {
  database: t('数据库'),
  storage: t('存储'),
  toolchain: t('工具链'),
  runtime: t('运行引擎'),
  library: t('离线资源'),
  config: t('配置'),
  network: t('网络')
}

const overallTone = computed(() => {
  const status = report.value?.status
  if (status === 'ok') return 'success'
  if (status === 'warn') return 'warning'
  return 'danger'
})

const overallTitle = computed(() => {
  const status = report.value?.status
  if (status === 'ok') return t('依赖全部就绪')
  if (status === 'warn') return t('存在需要注意的项')
  return t('存在阻塞问题')
})

const filteredItems = computed(() => {
  const items = report.value?.items || []
  if (filter.value === 'all') return items
  return items.filter((item) => item.status === filter.value)
})

const grouped = computed(() => {
  const map = new Map<string, CheckItem[]>()
  for (const item of filteredItems.value) {
    if (!map.has(item.category)) map.set(item.category, [])
    map.get(item.category)!.push(item)
  }
  return Array.from(map.entries())
})

function toneFor(status: string) {
  if (status === 'ok') return 'success'
  if (status === 'warn') return 'warning'
  return 'danger'
}

function iconFor(status: string) {
  if (status === 'ok') return 'check'
  if (status === 'warn') return 'warning'
  return 'close'
}

async function run() {
  loading.value = true
  try {
    const params = new URLSearchParams({ deep: deep.value ? '1' : '0', network: network.value ? '1' : '0' })
    report.value = await api.get<Report>(`/api/v1/health/check?${params.toString()}`)
  } catch (error) {
    app.fromError(error, t('依赖自检失败'))
  } finally {
    loading.value = false
  }
}

async function fix(item: CheckItem) {
  if (!item.fix) return
  busy.value = item.id
  try {
    const result = await api.post<Record<string, any>>('/api/v1/health/fix', { action: item.fix })
    if (result.report) report.value = result.report as Report
    app.toast('success', t('修复已执行'), result.message || '')
  } catch (error) {
    app.fromError(error, t('修复失败'))
  } finally {
    busy.value = ''
  }
}

async function fixAll() {
  const actions = Array.from(new Set((report.value?.items || []).filter((item) => item.fixable).map((item) => item.fix)))
  if (!actions.length) {
    app.toast('info', t('没有可自动修复的项'))
    return
  }
  busy.value = 'all'
  let applied = 0
  try {
    for (const action of actions) {
      const result = await api.post<Record<string, any>>('/api/v1/health/fix', { action })
      if (result.report) report.value = result.report as Report
      applied += 1
    }
    app.toast('success', t('已执行 {0} 项修复', [applied]), t('请重新运行自检确认结果'))
  } catch (error) {
    app.fromError(error, t('批量修复失败'))
  } finally {
    busy.value = ''
  }
}

onMounted(async () => {
  await app.init()
  await run()
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('依赖健康自检') }}</h1>
        <p class="page-subtitle">
          {{ t('检查数据库、目录权限、磁盘空间、Python 转换依赖、llama.cpp 各后端与离线资源完整性；默认完全离线运行。') }}
        </p>
      </div>
      <div class="page-actions">
        <button class="btn" :disabled="busy === 'all'" @click="fixAll">
          <span v-if="busy === 'all'" class="spinner"></span><Icon v-else name="zap" :size="13" /> {{ t('一键修复可处理项') }}
        </button>
        <button class="btn btn-primary" :disabled="loading" @click="run">
          <span v-if="loading" class="spinner"></span><Icon v-else name="activity" :size="13" /> {{ t('重新检测') }}
        </button>
      </div>
    </div>

    <div class="card" style="margin-bottom: 14px">
      <div class="card-body">
        <div class="health-controls">
          <div class="segmented">
            <button class="segmented-item" :class="{ active: filter === 'all' }" @click="filter = 'all'">{{ t('全部') }}</button>
            <button class="segmented-item" :class="{ active: filter === 'warn' }" @click="filter = 'warn'">{{ t('警告') }}</button>
            <button class="segmented-item" :class="{ active: filter === 'error' }" @click="filter = 'error'">{{ t('错误') }}</button>
          </div>
          <label class="check-line"><input v-model="deep" type="checkbox" /> {{ t('深度检测（重新探测工具版本，较慢）') }}</label>
          <label class="check-line"><input v-model="network" type="checkbox" /> {{ t('包含网络探测（校验 HF Token）') }}</label>
          <div class="toolbar-spacer"></div>
          <span v-if="report" class="small muted">{{ t('检测于 {0} · 耗时 {1} ms', [formatTime(report.checkedAt), report.elapsedMs]) }}</span>
        </div>
      </div>
    </div>

    <div v-if="loading && !report" class="loading-panel"><span class="spinner"></span> {{ t('正在执行依赖自检…') }}</div>

    <template v-else-if="report">
      <div class="notice" :class="overallTone" style="margin-bottom: 14px">
        <Icon :name="iconFor(report.status)" :size="15" class="notice-icon" />
        <div>
          <strong>{{ overallTitle }}</strong>
          <div class="small muted" style="margin-top: 2px">
            {{ t('{0} 项正常 · {1} 项警告 · {2} 项错误', [report.summary?.ok ?? 0, report.summary?.warn ?? 0, report.summary?.error ?? 0]) }}
            <span v-if="report.networkChecked"> · {{ t('已包含网络探测') }}</span>
          </div>
        </div>
      </div>

      <div class="health-grid">
        <div v-for="[category, list] in grouped" :key="category" class="card">
          <div class="card-header">
            <div>
              <div class="card-title">{{ categoryLabels[category] || category }}</div>
              <div class="card-subtitle">{{ t('{0} 个检查项', [list.length]) }}</div>
            </div>
            <span class="badge no-dot" :class="list.some((item) => item.status === 'error') ? 'danger' : list.some((item) => item.status === 'warn') ? 'warning' : 'success'">
              {{ t('{0}', [list.some((item) => item.status === 'error') ? t('存在错误') : list.some((item) => item.status === 'warn') ? t('需注意') : t('正常')]) }}
            </span>
          </div>
          <div class="card-body">
            <div v-for="item in list" :key="item.id" class="health-row">
              <span class="check-mark" :class="item.status === 'ok' ? 'pass' : 'fail'">
                <Icon :name="iconFor(item.status)" :size="9" :stroke="2" />
              </span>
              <div class="health-main">
                <div class="health-title">
                  {{ item.title }}
                  <span v-if="item.status !== 'ok'" class="badge no-dot" :class="toneFor(item.status)">
                    {{ t('{0}', [item.status === 'error' ? t('错误') : t('警告')]) }}
                  </span>
                </div>
                <div class="health-detail mono">{{ item.detail }}</div>
                <div v-if="item.hint" class="health-hint">{{ item.hint }}</div>
              </div>
              <button v-if="item.fixable" class="btn btn-sm" :disabled="busy === item.id" @click="fix(item)">
                <span v-if="busy === item.id" class="spinner"></span>{{ t('修复') }}
              </button>
            </div>
          </div>
        </div>
      </div>

      <div class="card" style="margin-top: 14px">
        <div class="card-header">
          <div>
            <div class="card-title">{{ t('运行环境') }}</div>
            <div class="card-subtitle">{{ t('仅用于定位问题，不会上传任何信息') }}</div>
          </div>
        </div>
        <div class="card-body">
          <div class="grid grid-2">
            <div class="kv"><span>{{ t('平台') }}</span><span>{{ report.environment?.os || '—' }} · {{ report.environment?.host || '—' }}</span></div>
            <div class="kv"><span>{{ t('版本') }}</span><span class="mono">{{ report.environment?.version || '—' }}</span></div>
            <div class="kv"><span>{{ t('内存') }}</span><span>{{ humanBytes(report.environment?.memoryAvailable) }} / {{ humanBytes(report.environment?.memoryTotal) }}</span></div>
            <div class="kv"><span>{{ t('深度检测') }}</span><span>{{ report.deep ? t('是') : t('否') }}</span></div>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>

<style scoped>
.health-controls {
  display: flex;
  align-items: center;
  gap: 14px;
  flex-wrap: wrap;
}
.health-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(360px, 1fr));
  gap: 14px;
}
.health-row {
  display: flex;
  gap: 10px;
  align-items: flex-start;
  padding: 8px 0;
  border-bottom: 1px solid var(--border, rgba(127, 127, 127, 0.12));
}
.health-row:last-child {
  border-bottom: 0;
}
.health-main {
  min-width: 0;
  flex: 1;
}
.health-title {
  font-size: 13px;
  font-weight: 600;
  display: flex;
  align-items: center;
  gap: 6px;
}
.health-detail {
  font-size: 11px;
  color: var(--muted, #8a93a6);
  margin-top: 2px;
  word-break: break-all;
}
.health-hint {
  font-size: 11px;
  color: var(--muted, #8a93a6);
  margin-top: 3px;
  line-height: 1.5;
}
</style>
