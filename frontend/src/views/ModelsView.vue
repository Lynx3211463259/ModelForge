<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, formatTime, humanBytes, humanNumber, relativeTime } from '../api/client'
import { useRoute } from 'vue-router'
import type { Model, ModelDeleteResult } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import AppModal from '../components/AppModal.vue'
import DirectoryPicker from '../components/DirectoryPicker.vue'
import EmptyState from '../components/EmptyState.vue'
import StatusBadge from '../components/StatusBadge.vue'

const app = useAppStore()
const route = useRoute()
const models = ref<Model[]>([])
const loading = ref(true)
const query = ref('')
const onlyFavorites = ref(false)
const importOpen = ref(false)
const importPath = ref('')
const batchMode = ref(false)
const batchPaths = ref('')
const importing = ref(false)
const importSummary = ref<{ models: Model[]; errors: any[] } | null>(null)
const dirPickerOpen = ref(false)

function onImportDirectory(path: string) {
  if (batchMode.value) {
    const existing = batchPaths.value.split(/\r?\n/).map((line) => line.trim()).filter(Boolean)
    if (!existing.includes(path)) existing.push(path)
    batchPaths.value = existing.join('\n')
  } else {
    importPath.value = path
  }
}
const versionOpen = ref(false)
const versionLoading = ref(false)
const versionTree = ref<any>(null)
const versionModel = ref<Model | null>(null)

const filtered = computed(() => {
  const needle = query.value.trim().toLowerCase()
  return models.value.filter((model) => {
    if (onlyFavorites.value && !model.favorite) return false
    if (!needle) return true
    return `${model.name} ${model.architecture} ${model.path}`.toLowerCase().includes(needle)
  })
})

const stats = computed(() => ({
  total: models.value.length,
  ready: models.value.filter((item) => item.status === 'READY').length,
  size: models.value.reduce((sum, item) => sum + (item.totalSize || 0), 0),
  shards: models.value.reduce((sum, item) => sum + (item.shardCount || 0), 0)
}))

async function load() {
  loading.value = true
  try {
    models.value = await api.get<Model[]>('/api/v1/models?limit=500')
  } catch (error) {
    app.fromError(error, t('加载模型库失败'))
  } finally {
    loading.value = false
  }
}

async function openVersions(model: Model) {
  versionModel.value = model
  versionTree.value = null
  versionOpen.value = true
  versionLoading.value = true
  try {
    versionTree.value = await api.get<any>(`/api/v1/models/${model.id}/versions`)
  } catch (error) {
    app.fromError(error, t('加载版本树失败'))
  } finally {
    versionLoading.value = false
  }
}

async function copyText(value: string) {
  try {
    await navigator.clipboard.writeText(value)
    app.toast('success', t('已复制'))
  } catch {
    app.toast('warning', t('浏览器拒绝了剪贴板访问'))
  }
}

async function importModels() {
  const paths = batchMode.value
    ? batchPaths.value.split('\n').map((item) => item.trim()).filter(Boolean)
    : [importPath.value.trim()].filter(Boolean)
  if (!paths.length) {
    app.toast('warning', t('请输入模型目录路径'))
    return
  }
  importing.value = true
  importSummary.value = null
  try {
    const result = await api.post<{ models: Model[]; errors: any[] }>('/api/v1/models/import', { paths })
    importSummary.value = result
    if (result.models.length) {
      app.toast('success', t('已导入 {0} 个模型', [result.models.length]),
        result.errors.length ? t('{0} 个路径失败', [result.errors.length]) : t('扫描报告已生成'))
      await load()
      if (!result.errors.length) {
        importOpen.value = false
        importPath.value = ''
        batchPaths.value = ''
      }
    } else {
      app.toast('error', t('未导入模型'), result.errors[0]?.message || t('请检查路径和模型文件'))
    }
  } catch (error) {
    app.fromError(error, t('模型导入失败'))
  } finally {
    importing.value = false
  }
}

async function toggleFavorite(model: Model) {
  try {
    await api.patch(`/api/v1/models/${model.id}`, { favorite: !model.favorite })
    model.favorite = !model.favorite
  } catch (error) {
    app.fromError(error, t('更新收藏失败'))
  }
}

async function removeModel(model: Model) {
  if (!window.confirm(t('删除“{0}”及其磁盘模型文件？此操作不可恢复。', [model.name]))) return
  try {
    const result = await api.delete<ModelDeleteResult>(`/api/v1/models/${model.id}?deleteFiles=true`)
    models.value = models.value.filter((item) => item.id !== model.id)
    const detail = [t('释放 {0}', [result.freedLabel || humanBytes(result.freedBytes)]), ...(result.warnings || [])]
      .filter(Boolean)
      .join(' · ')
    if (result.removedFiles > 0) {
      app.toast('success', t('已删除模型及 {0} 个文件', [result.removedFiles]), detail)
    } else {
      app.toast('success', t('已删除模型记录（没有可删除的文件）'), detail)
    }
  } catch (error) {
    app.fromError(error, t('删除失败，模型记录已保留'))
  }
}

function shortArchitecture(model: Model) {
  return (model.architectureFamily || model.architecture || 'AI').slice(0, 2).toUpperCase()
}

onMounted(async () => {
  await load()
  // Deep link: /models?versions=<modelId> opens the version tree directly.
  const requested = String(route.query.versions || '')
  if (requested) {
    const target = models.value.find((item) => item.id === requested)
    if (target) await openVersions(target)
  }
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('模型库') }}</h1>
        <p class="page-subtitle">{{ t('以本地路径为源，不复制大模型文件。导入会解析 Safetensors 头部并生成完整性报告。') }}</p>
      </div>
      <div class="page-actions">
        <button class="btn" @click="load"><Icon name="refresh" :size="13" /> {{ t('刷新') }}</button>
        <button class="btn btn-primary" @click="importOpen = true"><Icon name="plus" :size="14" /> {{ t('导入模型') }}</button>
      </div>
    </div>

    <div class="grid grid-4" style="margin-bottom: 14px">
      <div class="card stat-card"><div class="stat-top"><span>{{ t('已登记模型') }}</span><Icon name="models" :size="13" /></div><div><div class="stat-value">{{ stats.total }}</div><div class="stat-note">{{ t('{0} 个模型检查就绪', [stats.ready]) }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('模型总体积') }}</span><Icon name="disk" :size="13" /></div><div><div class="stat-value" style="font-size: 21px">{{ humanBytes(stats.size) }}</div><div class="stat-note">{{ t('基于目录中的模型相关文件') }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('权重分片') }}</span><Icon name="layers" :size="13" /></div><div><div class="stat-value">{{ stats.shards }}</div><div class="stat-note">{{ t('已读取的 Safetensors 文件数') }}</div></div></div>
      <div class="card stat-card"><div class="stat-top"><span>{{ t('收藏') }}</span><Icon name="star" :size="13" /></div><div><div class="stat-value">{{ models.filter(m => m.favorite).length }}</div><div class="stat-note">{{ t('常用模型置顶显示') }}</div></div></div>
    </div>

    <div class="toolbar">
      <div style="position: relative; width: 280px">
        <Icon name="search" :size="14" style="position: absolute; left: 10px; top: 10px; color: var(--faint)" />
        <input v-model="query" class="input" style="padding-left: 31px" :placeholder="t('搜索名称、架构或路径')" />
      </div>
      <button class="chip" :class="{ active: onlyFavorites }" @click="onlyFavorites = !onlyFavorites">
        <Icon name="star" :size="12" /> {{ t('仅收藏') }}
      </button>
      <span class="small muted">{{ t('{0} 个结果', [filtered.length]) }}</span>
      <div class="toolbar-spacer"></div>
      <span class="tag"><Icon name="info" :size="10" style="margin-right: 4px" /> {{ t('本地优先模式') }}</span>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取模型库…') }}</div>

    <div v-else-if="!filtered.length" class="card">
      <EmptyState icon="models" :title="t('没有匹配的模型')" copy="导入一个包含 config.json、Safetensors 权重与 Tokenizer 文件的完整模型目录。ModelForge 不会一次性把权重载入内存。">
        <button class="btn btn-primary" @click="importOpen = true"><Icon name="plus" :size="13" /> {{ t('导入模型目录') }}</button>
      </EmptyState>
    </div>

    <div v-else class="grid grid-3">
      <article v-for="model in filtered" :key="model.id" class="card model-card">
        <div class="model-card-top">
          <div class="model-avatar">{{ shortArchitecture(model) }}</div>
          <div style="min-width: 0; flex: 1">
            <div class="model-name truncate">{{ model.name }}</div>
            <div class="model-path truncate mono" :title="model.path">{{ model.path }}</div>
          </div>
          <button class="icon-btn" :title="model.favorite ? t('取消收藏') : t('收藏')" @click.stop="toggleFavorite(model)">
            <Icon name="star" :size="14" :style="{ color: model.favorite ? 'var(--amber)' : '' }" />
          </button>
        </div>

        <div class="model-tags">
          <span class="tag">{{ model.architectureFamily || 'Unknown' }}</span>
          <span class="tag">{{ model.architecture || '—' }}</span>
          <span v-if="model.dtype" class="tag">{{ model.dtype }}</span>
          <StatusBadge :status="model.status" />
        </div>

        <div class="model-meta">
          <div class="model-meta-item"><span class="model-meta-label">{{ t('参数') }}</span><span class="model-meta-value">{{ humanNumber(model.parameterCount) }}</span></div>
          <div class="model-meta-item"><span class="model-meta-label">{{ t('体积') }}</span><span class="model-meta-value">{{ humanBytes(model.totalSize) }}</span></div>
          <div class="model-meta-item"><span class="model-meta-label">{{ t('分片') }}</span><span class="model-meta-value">{{ model.shardCount }}</span></div>
        </div>

        <div style="display: flex; align-items: center; gap: 6px; margin-top: 14px">
          <router-link :to="{ path: '/analyzer', query: { id: model.id } }" class="btn btn-sm" style="flex: 1">
            <Icon name="analyzer" :size="12" /> {{ t('分析') }}
          </router-link>
          <router-link :to="{ path: '/convert', query: { model: model.id } }" class="btn btn-sm btn-primary" style="flex: 1">
            {{ t('转换') }} <Icon name="arrow" :size="12" />
          </router-link>
          <button class="icon-btn" :title="t('版本树')" @click="openVersions(model)"><Icon name="layers" :size="13" /></button>
          <button class="icon-btn" :title="t('删除模型及磁盘文件')" @click="removeModel(model)"><Icon name="trash" :size="13" /></button>
        </div>
        <div class="faint tiny" style="margin-top: 9px">{{ t('导入于 {0}', [relativeTime(model.createdAt)]) }}</div>
      </article>
    </div>

    <DirectoryPicker v-model="dirPickerOpen" :initial-path="batchMode ? '' : importPath" @select="onImportDirectory" />

    <AppModal v-model="importOpen" :title="t('导入本地模型')" subtitle="ModelForge 读取路径，不将权重上传到浏览器或外部服务。" :wide="batchMode">
      <div class="tabs">
        <button class="tab" :class="{ active: !batchMode }" @click="batchMode = false">{{ t('单个目录') }}</button>
        <button class="tab" :class="{ active: batchMode }" @click="batchMode = true">{{ t('批量目录') }}</button>
      </div>

      <div v-if="!batchMode">
        <div class="field">
          <label class="field-label">{{ t('模型目录的绝对路径') }}</label>
          <div class="input-with-button">
            <input v-model="importPath" class="input mono" :placeholder="t('例如 C:\\models\\Qwen3-0.6B')" @keyup.enter="importModels" />
            <button class="btn btn-sm" @click="dirPickerOpen = true"><Icon name="folder" :size="12" /> {{ t('浏览') }}</button>
          </div>
          <div class="field-hint">{{ t('目录应包含 config.json、一个或多个 .safetensors 文件及 Tokenizer。支持分片索引与多分片导入。') }}</div>
        </div>
      </div>
      <div v-else>
        <div class="field">
          <label class="field-label">{{ t('每行一个模型目录') }}</label>
          <textarea v-model="batchPaths" class="textarea" style="min-height: 150px" placeholder="C:\models\ModelA&#10;D:\models\ModelB"></textarea>
          <button class="btn btn-sm" style="margin-top: 8px" @click="dirPickerOpen = true"><Icon name="folder" :size="12" /> {{ t('浏览并添加目录') }}</button>
        </div>
      </div>

      <div class="notice info">
        <Icon name="info" :size="14" class="notice-icon" />
        <div>{{ t('扫描只读取 Safetensors 头部和配置文件，不会把大权重读入内存。缺少 Tokenizer 或分片的模型会被标记为不完整并阻止转换。') }}</div>
      </div>

      <div v-if="importSummary?.errors?.length" class="notice danger" style="margin-top: 10px">
        <Icon name="warning" :size="14" class="notice-icon" />
        <div>
          <div v-for="err in importSummary.errors" :key="err.path" style="margin-bottom: 2px">
            <span class="mono small">{{ err.path }}</span><br />{{ err.message }}
          </div>
        </div>
      </div>

      <template #footer>
        <button class="btn" @click="importOpen = false">{{ t('取消') }}</button>
        <button class="btn btn-primary" :disabled="importing" @click="importModels">
          <span v-if="importing" class="spinner"></span> {{ t('{0}', [importing ? t('扫描中…') : t('扫描并导入')]) }}</button>
      </template>
    </AppModal>

    <AppModal v-model="versionOpen" :title="t('模型版本树')" :subtitle="t('源模型与它为根产生的每个 GGUF 产物，可追溯到具体任务与校验值。')" :wide="true">
      <div v-if="versionLoading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取版本树…') }}</div>
      <template v-else-if="versionTree">
        <div class="grid grid-3" style="margin-bottom: 12px">
          <div class="card stat-card">
            <div class="stat-top"><span>{{ t('产物数量') }}</span><Icon name="layers" :size="13" /></div>
            <div><div class="stat-value">{{ versionTree.totals?.count || 0 }}</div><div class="stat-note">{{ t('每个量化产物一条记录') }}</div></div>
          </div>
          <div class="card stat-card">
            <div class="stat-top"><span>{{ t('产物总计') }}</span><Icon name="disk" :size="13" /></div>
            <div><div class="stat-value">{{ humanBytes(versionTree.totals?.size || 0) }}</div><div class="stat-note">{{ t('按记录大小累加') }}</div></div>
          </div>
          <div class="card stat-card" :class="{ 'stat-accent': !(versionTree.totals?.missing || 0) }">
            <div class="stat-top"><span>{{ t('文件缺失') }}</span><Icon name="warning" :size="13" /></div>
            <div><div class="stat-value">{{ versionTree.totals?.missing || 0 }}</div><div class="stat-note">{{ t('磁盘上找不到的产物数') }}</div></div>
          </div>
        </div>

        <div class="version-tree">
          <div class="version-node version-root">
            <div class="version-rail"><span class="version-dot"></span></div>
            <div class="version-body">
              <div class="version-head">
                <span class="badge success">{{ t('源模型') }}</span>
                <strong class="truncate">{{ versionTree.root?.label }}</strong>
                <span class="small muted">{{ versionTree.root?.quantType || '—' }}</span>
                <span class="small muted" style="margin-left: auto">{{ humanBytes(versionTree.root?.size || 0) }}</span>
              </div>
              <div class="version-path mono truncate" :title="versionTree.root?.path">{{ versionTree.root?.path }}</div>
            </div>
          </div>

          <div v-for="node in versionTree.nodes.filter((item: any) => item.kind === 'artifact')" :key="node.id" class="version-node">
            <div class="version-rail"><span class="version-dot artifact"></span></div>
            <div class="version-body">
              <div class="version-head">
                <span class="badge" :class="node.exists ? 'info' : 'danger'">{{ node.quantType || 'GGUF' }}</span>
                <strong>{{ humanBytes(node.size || 0) }}</strong>
                <span class="tag">{{ node.createdAt ? formatTime(node.createdAt) : '—' }}</span>
                <span v-if="!node.exists" class="badge danger">{{ t('文件缺失') }}</span>
                <span style="margin-left: auto; display: flex; gap: 6px">
                  <router-link v-if="node.taskId" :to="{ path: '/tasks', query: { task: node.taskId } }" class="btn btn-sm">
                    <Icon name="tasks" :size="12" /> {{ t('查看任务') }}
                  </router-link>
                  <a v-if="node.taskId && node.exists" class="btn btn-sm" :href="`/api/v1/conversions/${node.taskId}/download`">
                    <Icon name="download" :size="12" /> {{ t('下载') }}
                  </a>
                  <button class="icon-btn" :title="t('复制路径')" @click="copyText(node.path)"><Icon name="copy" :size="12" /></button>
                </span>
              </div>
              <div class="version-path mono truncate" :title="node.path">{{ node.path }}</div>
              <div class="version-meta">
                <span class="mono">{{ node.sha256 ? 'sha256 ' + node.sha256.slice(0, 16) + '…' : t('无远端哈希') }}</span>
                <span v-if="node.toolVersion" class="mono truncate" :title="node.toolVersion">{{ node.toolVersion }}</span>
              </div>
            </div>
          </div>
        </div>

        <EmptyState
          v-if="!(versionTree.totals?.count || 0)"
          icon="layers"
          :title="t('还没有产物')"
          :copy="t('为该模型创建转换任务后，产物会以树的形式出现在这里。')"
        />
      </template>
      <template #footer>
        <router-link v-if="versionModel" :to="{ path: '/convert', query: { model: versionModel.id } }" class="btn">
          <Icon name="convert" :size="13" /> {{ t('转换此模型') }}
        </router-link>
        <button class="btn btn-primary" @click="versionOpen = false">{{ t('关闭') }}</button>
      </template>
    </AppModal>
  </div>
</template>
