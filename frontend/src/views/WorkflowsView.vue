<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref } from 'vue'
import { api, humanBytes } from '../api/client'
import type { Model } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import AppModal from '../components/AppModal.vue'
import EmptyState from '../components/EmptyState.vue'

const app = useAppStore()
const workflows = ref<any[]>([])
const models = ref<Model[]>([])
const selectedModel = ref('')
const loading = ref(true)
const modalOpen = ref(false)
const editingId = ref('')
const saving = ref(false)
const importInput = ref<HTMLInputElement | null>(null)
const form = ref({
  name: '',
  description: '',
  quantTypes: [] as string[],
  outType: 'f16',
  outputDir: '',
  threads: 4,
  overwrite: false,
  computeSha256: true
})

const quantOptions = computed(() => {
  const types = app.capabilities?.quantTypes || []
  return types.filter((item) => !['F32', 'F16', 'BF16', 'COPY'].includes(item.toUpperCase()))
})

async function load() {
  loading.value = true
  try {
    const [workflowData, modelData, settings] = await Promise.all([
      api.get<any[]>('/api/v1/workflows'),
      api.get<Model[]>('/api/v1/models?limit=500'),
      api.get<any>('/api/v1/settings')
    ])
    workflows.value = workflowData
    models.value = modelData
    if (models.value.length && !selectedModel.value) selectedModel.value = models.value[0].id
    form.value.outputDir = settings.defaultOutputDir
    form.value.threads = settings.threads
  } catch (error) {
    app.fromError(error, t('加载工作流失败'))
  } finally {
    loading.value = false
  }
}

function openCreate() {
  editingId.value = ''
  form.value = {
    name: '',
    description: '',
    quantTypes: ['Q4_K_M'].filter((item) => quantOptions.value.includes(item)),
    outType: 'f16',
    outputDir: app.settings?.defaultOutputDir || '',
    threads: app.settings?.threads || 4,
    overwrite: false,
    computeSha256: true
  }
  modalOpen.value = true
}

function openEdit(workflow: any) {
  editingId.value = workflow.id
  const definition = workflow.definition || {}
  form.value = {
    name: workflow.name,
    description: workflow.description || '',
    quantTypes: definition.quantTypes || [],
    outType: definition.outType || 'f16',
    outputDir: definition.outputDir || app.settings?.defaultOutputDir || '',
    threads: definition.threads || app.settings?.threads || 4,
    overwrite: Boolean(definition.overwrite),
    computeSha256: definition.computeSha256 !== false
  }
  modalOpen.value = true
}

function toggleQuant(type: string) {
  if (form.value.quantTypes.includes(type)) form.value.quantTypes = form.value.quantTypes.filter((item) => item !== type)
  else form.value.quantTypes = [...form.value.quantTypes, type]
}

async function save() {
  if (!form.value.name.trim()) {
    app.toast('warning', t('请输入工作流名称'))
    return
  }
  if (!form.value.quantTypes.length) {
    app.toast('warning', t('至少选择一个量化类型'))
    return
  }
  saving.value = true
  const payload = {
    name: form.value.name,
    description: form.value.description,
    definition: {
      quantTypes: form.value.quantTypes,
      outType: form.value.outType,
      outputDir: form.value.outputDir,
      threads: Number(form.value.threads),
      overwrite: form.value.overwrite,
      computeSha256: form.value.computeSha256
    }
  }
  try {
    if (editingId.value) await api.put(`/api/v1/workflows/${editingId.value}`, payload)
    else await api.post('/api/v1/workflows', payload)
    app.toast('success', editingId.value ? t('工作流已更新') : t('工作流已创建'))
    modalOpen.value = false
    await load()
  } catch (error) {
    app.fromError(error, t('保存工作流失败'))
  } finally {
    saving.value = false
  }
}

async function run(workflow: any) {
  if (!selectedModel.value) {
    app.toast('warning', t('请先选择要运行的模型'))
    return
  }
  try {
    const result = await api.post<{ created: any[]; errors: any[] }>(`/api/v1/workflows/${workflow.id}/run`, { modelId: selectedModel.value })
    if (result.created?.length) app.toast('success', t('工作流已提交'), t('{0} 个转换任务已进入队列', [result.created.length]))
    if (result.errors?.length) app.toast('warning', t('{0} 个步骤未能创建', [result.errors.length]), result.errors[0]?.message)
  } catch (error) {
    app.fromError(error, t('运行工作流失败'))
  }
}

async function remove(workflow: any) {
  if (!window.confirm(t('删除工作流“{0}”？已有任务不受影响。', [workflow.name]))) return
  try {
    await api.delete(`/api/v1/workflows/${workflow.id}`)
    await load()
  } catch (error) {
    app.fromError(error, t('删除工作流失败'))
  }
}

function exportWorkflow(workflow: any) {
  const blob = new Blob([JSON.stringify(workflow, null, 2)], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = `${workflow.name}.modelforge-workflow.json`
  link.click()
  URL.revokeObjectURL(url)
}

async function importWorkflow(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return
  try {
    const parsed = JSON.parse(await file.text())
    const definition = parsed.definition || parsed
    await api.post('/api/v1/workflows', {
      name: parsed.name || file.name.replace('.json', ''),
      description: parsed.description || 'Imported workflow',
      definition
    })
    app.toast('success', t('工作流已导入'))
    await load()
  } catch (error) {
    app.fromError(error, t('导入工作流失败'))
  } finally {
    input.value = ''
  }
}

onMounted(async () => { await app.init(); await load() })
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('工作流') }}</h1>
        <p class="page-subtitle">{{ t('把模型和多个量化方案组合成可复用流水线。每个步骤都是真实转换任务，可单独查看日志。') }}</p>
      </div>
      <div class="page-actions">
        <select v-if="models.length" v-model="selectedModel" class="select" style="width: 240px">
          <option v-for="model in models" :key="model.id" :value="model.id">{{ model.name }}</option>
        </select>
        <button class="btn" @click="importInput?.click()"><Icon name="upload" :size="13" /> {{ t('导入') }}</button>
        <button class="btn btn-primary" @click="openCreate"><Icon name="plus" :size="13" /> {{ t('新建工作流') }}</button>
        <input ref="importInput" type="file" accept="application/json" style="display: none" @change="importWorkflow" />
      </div>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取工作流…') }}</div>
    <div v-else-if="!workflows.length" class="card">
      <EmptyState icon="workflows" :title="t('还没有工作流')" copy="创建一个“模型 → BF16 → Q4_K_M → 验证 → 报告”的流水线，之后可对不同模型复用。">
        <button class="btn btn-primary" @click="openCreate">{{ t('创建第一个工作流') }}</button>
      </EmptyState>
    </div>

    <div v-else class="grid grid-2">
      <article v-for="workflow in workflows" :key="workflow.id" class="card">
        <div class="card-header">
          <div class="workflow-mark"><Icon name="workflows" :size="17" /></div>
          <div style="min-width: 0; flex: 1">
            <div class="card-title">{{ workflow.name }}</div>
            <div class="card-subtitle">{{ t('{0}', [workflow.description || t('未填写描述')]) }}</div>
          </div>
          <span class="badge" :class="workflow.enabled ? 'success' : 'muted'">{{ t('{0}', [workflow.enabled ? t('启用') : t('停用')]) }}</span>
        </div>
        <div class="card-body">
          <div class="workflow-line">
            <span class="workflow-node">MODEL</span>
            <span class="workflow-arrow">→</span>
            <span class="workflow-node">SCAN</span>
            <span class="workflow-arrow">→</span>
            <span class="workflow-node">{{ (workflow.definition?.outType || 'f16').toUpperCase() }} GGUF</span>
            <span class="workflow-arrow">→</span>
            <span class="workflow-node accent">{{ (workflow.definition?.quantTypes || []).join(' / ') || 'NO QUANT' }}</span>
            <span class="workflow-arrow">→</span>
            <span class="workflow-node">VERIFY</span>
          </div>

          <div class="kv-grid" style="margin-top: 13px">
            <div class="kv"><span>{{ t('输出目录') }}</span><span>{{ workflow.definition?.outputDir || app.settings?.defaultOutputDir }}</span></div>
            <div class="kv"><span>{{ t('线程') }}</span><span>{{ workflow.definition?.threads || app.settings?.threads }}</span></div>
            <div class="kv"><span>{{ t('覆盖输出') }}</span><span>{{ t('{0}', [workflow.definition?.overwrite ? t('是') : t('否')]) }}</span></div>
            <div class="kv"><span>SHA256</span><span>{{ t('{0}', [workflow.definition?.computeSha256 === false ? t('关闭') : t('开启')]) }}</span></div>
          </div>

          <div class="btn-row" style="margin-top: 14px">
            <button class="btn btn-primary" :disabled="!selectedModel || !workflow.definition?.quantTypes?.length" @click="run(workflow)">
              <Icon name="play" :size="12" /> {{ t('运行') }}
            </button>
            <button class="btn btn-sm" @click="openEdit(workflow)">{{ t('编辑') }}</button>
            <button class="btn btn-sm" @click="exportWorkflow(workflow)">{{ t('导出') }}</button>
            <button class="btn btn-sm btn-danger" @click="remove(workflow)">{{ t('删除') }}</button>
            <router-link to="/tasks" class="btn btn-sm btn-ghost" style="margin-left: auto">{{ t('任务队列') }} <Icon name="arrow" :size="11" /></router-link>
          </div>
        </div>
      </article>
    </div>

    <div class="notice info" style="margin-top: 14px">
      <Icon name="info" :size="14" class="notice-icon" />
      <div>{{ t('工作流中的每个量化步骤会创建独立转换任务，因此可分别重试、取消与审计。使用并发上限 1 可以避免多个大模型同时转换导致内存压力。') }}</div>
    </div>

    <AppModal v-model="modalOpen" :title="editingId ? t('编辑工作流') : t('新建工作流')" subtitle="定义模型转换与量化方案" wide>
      <div class="field-row">
        <div class="field"><label class="field-label">{{ t('名称') }}</label><input v-model="form.name" class="input" :placeholder="t('例如：Qwen 通用量化套件')" /></div>
        <div class="field"><label class="field-label">{{ t('输出目录') }}</label><input v-model="form.outputDir" class="input mono" /></div>
      </div>
      <div class="field"><label class="field-label">{{ t('描述') }}</label><input v-model="form.description" class="input" :placeholder="t('可选说明')" /></div>

      <div class="section-label">{{ t('量化步骤') }}</div>
      <div v-if="!quantOptions.length" class="notice warning"><Icon name="warning" :size="14" class="notice-icon" />{{ t('当前没有可用量化类型，请先配置 llama-quantize。') }}</div>
      <div v-else style="display: flex; flex-wrap: wrap; gap: 6px">
        <button v-for="quant in quantOptions" :key="quant" class="chip" :class="{ active: form.quantTypes.includes(quant) }" @click="toggleQuant(quant)">{{ quant }}</button>
      </div>

      <div class="field-row" style="margin-top: 16px">
        <div class="field">
          <label class="field-label">{{ t('中间精度') }}</label>
          <select v-model="form.outType" class="select"><option value="f16">F16</option><option value="bf16">BF16</option><option value="f32">F32</option></select>
        </div>
        <div class="field">
          <label class="field-label">{{ t('线程数') }}</label>
          <input v-model.number="form.threads" type="number" min="0" max="512" class="input" />
        </div>
      </div>
      <div class="grid grid-2">
        <label class="check"><input v-model="form.overwrite" type="checkbox" /><span>{{ t('覆盖已有输出') }}</span></label>
        <label class="check"><input v-model="form.computeSha256" type="checkbox" /><span>{{ t('计算 SHA256') }}</span></label>
      </div>
      <div class="notice info" style="margin-top: 14px">
        <Icon name="info" :size="14" class="notice-icon" />
        <div>{{ t('工作流模板只包含真实可执行步骤。每个量化类型会依次进入任务队列，并生成独立报告。') }}</div>
      </div>
      <template #footer>
        <button class="btn" @click="modalOpen = false">{{ t('取消') }}</button>
        <button class="btn btn-primary" :disabled="saving" @click="save"><span v-if="saving" class="spinner"></span> {{ t('保存工作流') }}</button>
      </template>
    </AppModal>
  </div>
</template>

<style scoped>
.workflow-mark {
  width: 34px;
  height: 34px;
  border-radius: 9px;
  border: 1px solid rgba(91, 149, 255, 0.2);
  background: var(--blue-dim);
  color: var(--blue);
  display: grid;
  place-items: center;
}
.workflow-line {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-wrap: wrap;
}
.workflow-node {
  border: 1px solid var(--line);
  background: rgba(255, 255, 255, 0.02);
  border-radius: 6px;
  padding: 4px 7px;
  font-family: var(--font-mono);
  font-size: 9px;
  color: var(--muted);
}
.workflow-node.accent {
  color: var(--accent);
  border-color: rgba(52, 214, 196, 0.22);
  background: var(--accent-dim);
}
.workflow-arrow {
  color: var(--faint);
  font-size: 10px;
}
</style>
