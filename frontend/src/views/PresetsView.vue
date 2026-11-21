<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api, formatTime } from '../api/client'
import { t } from '../i18n'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import AppModal from '../components/AppModal.vue'
import EmptyState from '../components/EmptyState.vue'

interface Profile {
  id: string
  name: string
  description: string
  builtin: boolean
  params: Record<string, any>
  createdAt: string
}

const router = useRouter()
const app = useAppStore()

const profiles = ref<Profile[]>([])
const loading = ref(true)
const saving = ref(false)
const editorOpen = ref(false)
const editingId = ref('')
const form = reactive({
  name: '',
  description: '',
  outType: 'f16',
  quantType: '',
  threads: 4,
  verify: true,
  computeSha256: true
})

const builtinCount = computed(() => profiles.value.filter((item) => item.builtin).length)
const customCount = computed(() => profiles.value.length - builtinCount.value)
const quantTypes = computed<string[]>(() => app.capabilities?.quantTypes || [])
const outTypes = [
  { id: 'f16', label: 'F16' },
  { id: 'bf16', label: 'BF16' },
  { id: 'f32', label: 'F32' }
]

function resetForm() {
  form.name = ''
  form.description = ''
  form.outType = 'f16'
  form.quantType = quantTypes.value.includes('Q4_K_M') ? 'Q4_K_M' : quantTypes.value[0] || ''
  form.threads = app.settings?.threads || 4
  form.verify = true
  form.computeSha256 = true
}

async function load() {
  loading.value = true
  try {
    profiles.value = await api.get<Profile[]>('/api/v1/quantization/profiles')
  } catch (error) {
    app.fromError(error, t('加载预设失败'))
  } finally {
    loading.value = false
  }
}

function openCreate() {
  editingId.value = ''
  resetForm()
  editorOpen.value = true
}

function openEdit(profile: Profile) {
  editingId.value = profile.id
  form.name = profile.name
  form.description = profile.description || ''
  form.outType = String(profile.params?.outType || 'f16')
  form.quantType = String(profile.params?.quantType || '')
  form.threads = Number(profile.params?.threads ?? (app.settings?.threads || 4))
  form.verify = profile.params?.verify !== false
  form.computeSha256 = profile.params?.computeSha256 !== false
  editorOpen.value = true
}

async function save() {
  if (!form.name.trim()) {
    app.toast('warning', t('请输入预设名称'))
    return
  }
  saving.value = true
  const payload = {
    name: form.name.trim(),
    description: form.description.trim(),
    params: {
      outType: form.outType,
      quantType: form.quantType,
      threads: Number(form.threads) || 0,
      verify: form.verify,
      computeSha256: form.computeSha256
    }
  }
  try {
    if (editingId.value) await api.put(`/api/v1/quantization/profiles/${editingId.value}`, payload)
    else await api.post('/api/v1/quantization/profiles', payload)
    app.toast('success', editingId.value ? t('预设已更新') : t('预设已创建'), form.name.trim())
    editorOpen.value = false
    await load()
  } catch (error) {
    app.fromError(error, t('保存预设失败'))
  } finally {
    saving.value = false
  }
}

async function remove(profile: Profile) {
  if (!window.confirm(t('删除预设“{0}”？已有任务不受影响。', [profile.name]))) return
  try {
    await api.delete(`/api/v1/quantization/profiles/${profile.id}`)
    app.toast('success', t('预设已删除'))
    await load()
  } catch (error) {
    app.fromError(error, t('删除预设失败'))
  }
}

function useForConversion(profile: Profile) {
  router.push({ path: '/convert', query: { preset: profile.id } })
}

function summarize(profile: Profile): string {
  const params = profile.params || {}
  const parts = [String(params.outType || 'f16').toUpperCase()]
  if (params.quantType) parts.push(String(params.quantType))
  if (params.threads) parts.push(`${params.threads} ${t('线程')}`)
  if (params.verify === false) parts.push(t('跳过验证'))
  return parts.join(' · ')
}

onMounted(async () => {
  await app.init()
  resetForm()
  await load()
})
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('配置预设') }}</h1>
        <p class="page-subtitle">
          {{ t('把常用的精度、量化类型与验证选项保存为预设，在转换页一键套用。内置预设来自产品默认值，不可修改或删除。') }}
        </p>
      </div>
      <div class="page-actions">
        <button class="btn" :disabled="loading" @click="load"><Icon name="refresh" :size="13" /> {{ t('刷新') }}</button>
        <button class="btn btn-primary" @click="openCreate"><Icon name="plus" :size="13" /> {{ t('新建预设') }}</button>
      </div>
    </div>

    <div class="grid grid-3" style="margin-bottom: 14px">
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('全部预设') }}</span><Icon name="layers" :size="13" /></div>
        <div><div class="stat-value">{{ profiles.length }}</div><div class="stat-note">{{ t('内置与自定义预设') }}</div></div>
      </div>
      <div class="card stat-card">
        <div class="stat-top"><span>{{ t('内置预设') }}</span><Icon name="settings" :size="13" /></div>
        <div><div class="stat-value">{{ builtinCount }}</div><div class="stat-note">{{ t('由产品默认值提供，只读') }}</div></div>
      </div>
      <div class="card stat-card stat-accent">
        <div class="stat-top"><span>{{ t('自定义预设') }}</span><Icon name="save" :size="13" /></div>
        <div><div class="stat-value">{{ customCount }}</div><div class="stat-note">{{ t('可编辑、可删除、可套用') }}</div></div>
      </div>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在读取预设…') }}</div>

    <EmptyState
      v-else-if="!profiles.length"
      :title="t('还没有预设')"
      :copy="t('创建第一个预设，把精度与量化方案固定下来。')"
      icon="layers"
    >
      <button class="btn btn-primary" @click="openCreate"><Icon name="plus" :size="13" /> {{ t('新建预设') }}</button>
    </EmptyState>

    <div v-else class="grid grid-3">
      <article v-for="profile in profiles" :key="profile.id" class="card model-card">
        <div class="card-header" style="padding: 0 0 10px">
          <div style="min-width: 0">
            <div class="card-title truncate">{{ t(profile.name) }}</div>
            <div class="card-subtitle">{{ summarize(profile) }}</div>
          </div>
          <span class="badge" :class="profile.builtin ? 'muted' : 'success'">
            {{ profile.builtin ? t('内置') : t('自定义') }}
          </span>
        </div>
        <p class="small muted" style="min-height: 34px; margin: 0 0 10px">
          {{ t(profile.description || '未填写描述') }}
        </p>
        <div class="kv"><span>{{ t('输出精度') }}</span><span class="mono">{{ String(profile.params?.outType || 'f16').toUpperCase() }}</span></div>
        <div class="kv"><span>{{ t('目标量化') }}</span><span class="mono">{{ profile.params?.quantType || t('不量化') }}</span></div>
        <div class="kv"><span>{{ t('线程') }}</span><span class="mono">{{ profile.params?.threads || t('默认线程') }}</span></div>
        <div class="kv"><span>{{ t('创建时间') }}</span><span>{{ profile.createdAt ? formatTime(profile.createdAt) : '—' }}</span></div>
        <div style="display: flex; align-items: center; gap: 6px; margin-top: 12px">
          <button class="btn btn-sm btn-primary" style="flex: 1" @click="useForConversion(profile)">
            <Icon name="convert" :size="12" /> {{ t('用于转换') }}
          </button>
          <button class="icon-btn" :title="t('编辑')" :disabled="profile.builtin" @click="openEdit(profile)">
            <Icon name="save" :size="13" />
          </button>
          <button class="icon-btn" :title="t('删除')" :disabled="profile.builtin" @click="remove(profile)">
            <Icon name="trash" :size="13" />
          </button>
        </div>
      </article>
    </div>

    <AppModal
      v-model="editorOpen"
      :title="editingId ? t('编辑预设') : t('新建预设')"
      :subtitle="t('预设只保存参数，不绑定具体模型；套用后仍会执行完整的前置检查。')"
    >
      <div class="field">
        <label class="field-label">{{ t('名称') }}</label>
        <input v-model="form.name" class="input" maxlength="120" :placeholder="t('例如：Qwen 通用量化套件')" />
      </div>
      <div class="field">
        <label class="field-label">{{ t('可选说明') }}</label>
        <input v-model="form.description" class="input" maxlength="1000" :placeholder="t('例如：适合 8GB 显存的质量优先方案')" />
      </div>
      <div class="field-row">
        <div class="field">
          <label class="field-label">{{ t('输出精度') }}</label>
          <select v-model="form.outType" class="select">
            <option v-for="item in outTypes" :key="item.id" :value="item.id">{{ item.label }}</option>
          </select>
        </div>
        <div class="field">
          <label class="field-label">{{ t('目标量化') }}</label>
          <select v-model="form.quantType" class="select">
            <option value="">{{ t('不量化') }}</option>
            <option v-for="item in quantTypes" :key="item" :value="item">{{ item }}</option>
          </select>
        </div>
      </div>
      <div class="field-row">
        <div class="field">
          <label class="field-label">{{ t('CPU 线程数') }}</label>
          <input v-model.number="form.threads" class="input" type="number" min="0" max="512" />
          <div class="field-hint">{{ t('0 表示使用系统默认线程') }}</div>
        </div>
        <div class="field">
          <label class="field-label">{{ t('验证选项') }}</label>
          <label class="check"><input v-model="form.verify" type="checkbox" /><span>{{ t('转换后结构验证') }}</span></label>
          <label class="check"><input v-model="form.computeSha256" type="checkbox" /><span>{{ t('计算 SHA256') }}</span></label>
        </div>
      </div>
      <template #footer>
        <button class="btn" @click="editorOpen = false">{{ t('取消') }}</button>
        <button class="btn btn-primary" :disabled="saving" @click="save">
          <span v-if="saving" class="spinner"></span><Icon v-else name="save" :size="13" /> {{ t('保存预设') }}
        </button>
      </template>
    </AppModal>
  </div>
</template>
