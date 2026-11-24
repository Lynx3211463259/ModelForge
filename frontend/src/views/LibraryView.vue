<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, humanBytes, formatTime } from '../api/client'
import { useAppStore } from '../stores/app'
import AppModal from '../components/AppModal.vue'
import DirectoryPicker from '../components/DirectoryPicker.vue'
import Icon from '../components/Icon.vue'

interface LibraryItem {
  id: string
  type: string
  name: string
  version: string
  path: string
  size: number
  fileCount: number
  present: boolean
  sizeMatches?: boolean
  onDiskBytes?: number
  onDiskFiles?: number
  source: string
  repoId: string
  revision: string
  updatedAt: string
}

const app = useAppStore()

const items = ref<LibraryItem[]>([])
const stats = ref<Record<string, any>>({})
const typeFilter = ref('all')
const query = ref('')
const loading = ref(false)
const busy = ref('')
const verifyResult = ref<Record<string, any> | null>(null)

const exportOpen = ref(false)
const exportForm = ref({ destination: '', name: '', copy: true, includeSha256: true, types: ['model', 'dataset', 'backend'] })
const pickerOpen = ref(false)

const importOpen = ref(false)
const importForm = ref({ manifestPath: '', basePath: '', verifySha256: true })
const importResult = ref<Record<string, any> | null>(null)
const manifestPickerOpen = ref(false)
const basePickerOpen = ref(false)

const typeLabels: Record<string, string> = {
  model: t('模型'),
  dataset: t('数据集'),
  backend: t('推理后端'),
  other: t('其他')
}

const filtered = computed(() => {
  const needle = query.value.trim().toLowerCase()
  return items.value.filter((item) => {
    if (typeFilter.value !== 'all' && item.type !== typeFilter.value) return false
    if (!needle) return true
    return (
      item.name.toLowerCase().includes(needle) ||
      item.path.toLowerCase().includes(needle) ||
      item.repoId.toLowerCase().includes(needle)
    )
  })
})

const totalBytes = computed(() => filtered.value.reduce((sum, item) => sum + (item.onDiskBytes ?? item.size ?? 0), 0))
const missingCount = computed(() => items.value.filter((item) => !item.present).length)

async function load() {
  loading.value = true
  try {
    const params = new URLSearchParams({ type: typeFilter.value, q: query.value.trim(), limit: '300' })
    const data = await api.get<{ items: LibraryItem[]; stats: Record<string, any> }>(`/api/v1/library?${params.toString()}`)
    items.value = data.items || []
    stats.value = data.stats || {}
  } catch (error) {
    app.fromError(error, t('读取离线资源库失败'))
  } finally {
    loading.value = false
  }
}

async function rescan() {
  busy.value = 'rescan'
  try {
    const result = await api.post<Record<string, any>>('/api/v1/library/rescan', { prune: true })
    stats.value = result
    app.toast('success', t('资源库扫描完成'), t('索引 {0} 项资源', [result.indexed ?? 0]))
    await load()
  } catch (error) {
    app.fromError(error, t('扫描失败'))
  } finally {
    busy.value = ''
  }
}

async function verify() {
  busy.value = 'verify'
  try {
    verifyResult.value = await api.post<Record<string, any>>('/api/v1/library/verify', {})
    const missing = verifyResult.value?.missing?.length || 0
    if (missing) app.toast('warning', t('发现 {0} 项失效资源', [missing]), t('这些条目指向的文件已不存在'))
    else app.toast('success', t('离线资源完整'), t('已检查 {0} 项', [verifyResult.value?.checked ?? 0]))
    await load()
  } catch (error) {
    app.fromError(error, t('校验失败'))
  } finally {
    busy.value = ''
  }
}

async function forget(item: LibraryItem) {
  if (!window.confirm(t('从资源库移除 {0}？磁盘文件会保留。', [item.name]))) return
  busy.value = item.id
  try {
    await api.delete(`/api/v1/library/${encodeURIComponent(item.id)}`)
    app.toast('success', t('已从资源库移除'), item.name)
    await load()
  } catch (error) {
    app.fromError(error, t('移除失败'))
  } finally {
    busy.value = ''
  }
}

function openExport() {
  exportForm.value = {
    destination: exportForm.value.destination || '',
    name: t('ModelForge 离线资源包'),
    copy: true,
    includeSha256: true,
    types: ['model', 'dataset', 'backend']
  }
  exportOpen.value = true
}

function toggleExportType(value: string) {
  const list = exportForm.value.types
  const index = list.indexOf(value)
  if (index >= 0) list.splice(index, 1)
  else list.push(value)
}

async function runExport() {
  if (!exportForm.value.destination.trim()) {
    app.toast('warning', t('请选择导出目录'))
    return
  }
  busy.value = 'export'
  try {
    const result = await api.post<Record<string, any>>('/api/v1/library/export', {
      destination: exportForm.value.destination.trim(),
      name: exportForm.value.name,
      copy: exportForm.value.copy,
      includeSha256: exportForm.value.includeSha256,
      types: exportForm.value.types
    })
    exportOpen.value = false
    app.toast(
      'success',
      t('离线资源包已导出'),
      t('{0} 项 · {1}{2}', [result.itemCount, humanBytes(result.totalSize), result.copied ? t(' · 已复制文件') : t(' · 仅清单')])
    )
    await load()
  } catch (error) {
    app.fromError(error, t('导出失败'))
  } finally {
    busy.value = ''
  }
}

function openImport() {
  importResult.value = null
  importOpen.value = true
}

async function runImport() {
  if (!importForm.value.manifestPath.trim()) {
    app.toast('warning', t('请选择资源包目录或清单文件'))
    return
  }
  busy.value = 'import'
  try {
    const result = await api.post<Record<string, any>>('/api/v1/library/import', {
      manifestPath: importForm.value.manifestPath.trim(),
      basePath: importForm.value.basePath.trim(),
      verifySha256: importForm.value.verifySha256
    })
    importResult.value = result
    const missing = result.missing?.length || 0
    app.toast(
      missing ? 'warning' : 'success',
      t('已导入 {0} 项资源', [result.importedCount ?? 0]),
      missing ? t('{0} 项文件缺失', [missing]) : ''
    )
    await load()
  } catch (error) {
    app.fromError(error, t('导入失败'))
  } finally {
    busy.value = ''
  }
}

onMounted(async () => {
  await app.init()
  await load()
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('离线资源库') }}</h1>
        <p class="page-subtitle">
          {{ t('统一索引本地模型、Hugging Face 数据集与 llama.cpp 后端；可导出带清单与校验值的离线资源包，在无网络机器上导入后直接使用。') }}
        </p>
      </div>
      <div class="page-actions">
        <button class="btn" :disabled="busy === 'rescan'" @click="rescan">
          <span v-if="busy === 'rescan'" class="spinner"></span><Icon v-else name="refresh" :size="13" /> {{ t('扫描资源') }}
        </button>
        <button class="btn" :disabled="busy === 'verify'" @click="verify">
          <span v-if="busy === 'verify'" class="spinner"></span><Icon v-else name="check" :size="13" /> {{ t('校验完整性') }}
        </button>
        <button class="btn" @click="openImport"><Icon name="upload" :size="13" /> {{ t('导入资源包') }}</button>
        <button class="btn btn-primary" @click="openExport"><Icon name="download" :size="13" /> {{ t('导出资源包') }}</button>
      </div>
    </div>

    <div class="grid grid-3" style="margin-bottom: 14px">
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('已索引资源') }}</span><Icon name="layers" :size="13" /></div>
        <div>
          <div class="stat-value">{{ stats.total || items.length }}</div>
          <div class="stat-note">{{ t('{0} · 数据集目录 {1}', [humanBytes(stats.bytes || 0), stats.datasetsDir || '—']) }}</div>
        </div>
      </div>
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('当前筛选占用') }}</span><Icon name="disk" :size="13" /></div>
        <div>
          <div class="stat-value">{{ humanBytes(totalBytes) }}</div>
          <div class="stat-note">{{ t('{0} 项符合条件', [filtered.length]) }}</div>
        </div>
      </div>
      <div class="card stat-card" :class="{ 'stat-accent': missingCount > 0 }">
        <div class="stat-top"><span>{{ t('失效条目') }}</span><Icon name="warning" :size="13" /></div>
        <div>
          <div class="stat-value">{{ missingCount }}</div>
          <div class="stat-note">{{ t('{0}', [missingCount ? t('路径已不存在，可重新扫描或移除') : t('全部资源均可在本地访问')]) }}</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-header">
        <div>
          <div class="card-title">{{ t('资源清单') }}</div>
          <div class="card-subtitle">{{ t('移除条目只删除索引，不会删除磁盘文件。') }}</div>
        </div>
        <div class="card-header-actions">
          <div class="segmented">
            <button
              v-for="option in ['all', 'model', 'dataset', 'backend']"
              :key="option"
              class="segmented-item"
              :class="{ active: typeFilter === option }"
              @click="typeFilter = option; load()"
            >
              {{ option === 'all' ? t('全部') : typeLabels[option] }}
            </button>
          </div>
          <input v-model="query" class="input mono" style="max-width: 220px" :placeholder="t('搜索名称 / 路径')" @keyup.enter="load" />
          <button class="btn btn-sm" :disabled="loading" @click="load"><Icon name="refresh" :size="12" /></button>
        </div>
      </div>
      <div class="card-body" :class="{ flush: true }">
        <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取资源库…') }}</div>
        <div v-else-if="!filtered.length" class="empty-state" style="padding: 42px 16px">
          <div class="empty-icon"><Icon name="layers" :size="22" /></div>
          <div class="empty-title">{{ t('资源库为空') }}</div>
          <div class="empty-copy">{{ t('点击“扫描资源”索引 models 与 datasets 目录，或从下载中心下载数据集自动登记。') }}</div>
        </div>
        <table v-else class="table">
          <thead>
            <tr>
              <th>{{ t('名称') }}</th>
              <th style="width: 100px">{{ t('类型') }}</th>
              <th style="width: 120px">{{ t('版本') }}</th>
              <th style="width: 110px" class="num">{{ t('大小') }}</th>
              <th style="width: 80px" class="num">{{ t('文件') }}</th>
              <th>{{ t('路径') }}</th>
              <th style="width: 110px">{{ t('状态') }}</th>
              <th style="width: 70px"></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="item in filtered" :key="item.id">
              <td>
                <div class="strong">{{ item.name || item.repoId || item.id }}</div>
                <div class="small muted">{{ item.repoId && item.repoId !== item.name ? item.repoId : item.source }}</div>
              </td>
              <td><span class="badge no-dot" :class="item.type === 'dataset' ? 'warning' : item.type === 'backend' ? 'info' : 'success'">{{ typeLabels[item.type] || item.type }}</span></td>
              <td class="mono small">{{ item.version || '—' }}</td>
              <td class="mono num">{{ humanBytes(item.onDiskBytes ?? item.size) }}</td>
              <td class="mono num">{{ item.onDiskFiles ?? item.fileCount }}</td>
              <td class="mono small muted" style="max-width: 320px; overflow: hidden; text-overflow: ellipsis" :title="item.path">{{ item.path }}</td>
              <td>
                <span class="badge no-dot" :class="item.present ? (item.sizeMatches === false ? 'warning' : 'success') : 'danger'">
                  {{ t('{0}', [item.present ? (item.sizeMatches === false ? t('大小不一致') : t('可用')) : t('缺失')]) }}
                </span>
                <div class="small muted">{{ formatTime(item.updatedAt) }}</div>
              </td>
              <td>
                <button class="btn btn-sm" :disabled="busy === item.id" @click="forget(item)"><Icon name="trash" :size="12" /></button>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <div v-if="verifyResult" class="notice" :class="verifyResult.missing?.length ? 'warning' : 'success'" style="margin-top: 14px">
      <Icon :name="verifyResult.missing?.length ? 'warning' : 'check'" :size="14" class="notice-icon" />
      <div>
        <strong>{{ t('完整性校验：') }}</strong>
        {{ t('已检查 {0} 项，{1} 项缺失，{2} 项大小不一致。', [verifyResult.checked ?? 0, verifyResult.missing?.length ?? 0, verifyResult.sizeDrift?.length ?? 0]) }}
        <div v-for="entry in verifyResult.missing || []" :key="entry.id" class="small muted mono" style="margin-top: 2px">
          {{ entry.name }} → {{ entry.path }}
        </div>
      </div>
    </div>

    <AppModal v-model="exportOpen" :title="t('导出离线资源包')" :subtitle="t('生成 manifest 与 SHA256SUMS；勾选复制文件后可整体拷贝到离线机器。')" :wide="true">
      <div class="field">
        <label class="field-label">{{ t('导出目录') }}</label>
        <div class="field-row">
          <input v-model="exportForm.destination" class="input mono" :placeholder="t('例如 D:/modelforge-offline')" />
          <button class="btn" @click="pickerOpen = true"><Icon name="folder" :size="13" /> {{ t('浏览') }}</button>
        </div>
      </div>
      <div class="field">
        <label class="field-label">{{ t('资源包名称') }}</label>
        <input v-model="exportForm.name" class="input" />
      </div>
      <div class="field">
        <label class="field-label">{{ t('包含类型') }}</label>
        <div class="btn-row">
          <button
            v-for="option in ['model', 'dataset', 'backend']"
            :key="option"
            class="btn btn-sm"
            :class="{ 'btn-primary': exportForm.types.includes(option) }"
            @click="toggleExportType(option)"
          >
            {{ typeLabels[option] }}
          </button>
        </div>
      </div>
      <div class="field-row">
        <label class="check-line"><input v-model="exportForm.copy" type="checkbox" /> {{ t('复制资源文件（否则只导出清单）') }}</label>
        <label class="check-line"><input v-model="exportForm.includeSha256" type="checkbox" /> {{ t('计算 SHA256 校验值') }}</label>
      </div>
      <div class="btn-row" style="margin-top: 14px">
        <button class="btn" @click="exportOpen = false">{{ t('取消') }}</button>
        <button class="btn btn-primary" :disabled="busy === 'export'" @click="runExport">
          <span v-if="busy === 'export'" class="spinner"></span><Icon v-else name="download" :size="13" /> {{ t('开始导出') }}
        </button>
      </div>
    </AppModal>

    <AppModal v-model="importOpen" :title="t('导入离线资源包')" :subtitle="t('清单中的路径默认相对资源包目录解析；跨盘符迁移时请填写资源包根目录。')" :wide="true">
      <div class="field">
        <label class="field-label">{{ t('资源包目录 / 清单文件') }}</label>
        <div class="field-row">
          <input v-model="importForm.manifestPath" class="input mono" :placeholder="t('D:/modelforge-offline 或 .../modelforge-offline-bundle.json')" />
          <button class="btn" @click="manifestPickerOpen = true"><Icon name="file" :size="13" /> {{ t('选择清单') }}</button>
        </div>
      </div>
      <div class="field">
        <label class="field-label">{{ t('资源根目录（可选）') }}</label>
        <div class="field-row">
          <input v-model="importForm.basePath" class="input mono" :placeholder="t('资源包被解压到别处时填写')" />
          <button class="btn" @click="basePickerOpen = true"><Icon name="folder" :size="13" /> {{ t('浏览') }}</button>
        </div>
      </div>
      <label class="check-line"><input v-model="importForm.verifySha256" type="checkbox" /> {{ t('逐文件校验 SHA256（较慢，但能发现损坏）') }}</label>
      <div v-if="importResult" class="notice success" style="margin-top: 12px">
        <Icon name="check" :size="14" class="notice-icon" />
        <div>
          {{ t('已导入 {0} 项；缺失 {1} 项；校验失败 {2} 项。', [importResult.importedCount ?? 0, importResult.missing?.length ?? 0, importResult.mismatched?.length ?? 0]) }}
        </div>
      </div>
      <div class="btn-row" style="margin-top: 14px">
        <button class="btn" @click="importOpen = false">{{ t('关闭') }}</button>
        <button class="btn btn-primary" :disabled="busy === 'import'" @click="runImport">
          <span v-if="busy === 'import'" class="spinner"></span><Icon v-else name="upload" :size="13" /> {{ t('开始导入') }}
        </button>
      </div>
    </AppModal>

    <DirectoryPicker v-model="pickerOpen" :initial-path="exportForm.destination" @select="exportForm.destination = $event" />
    <DirectoryPicker v-model="manifestPickerOpen" mode="file" :initial-path="importForm.manifestPath" @select="importForm.manifestPath = $event" />
    <DirectoryPicker v-model="basePickerOpen" :initial-path="importForm.basePath" @select="importForm.basePath = $event" />
  </div>
</template>
