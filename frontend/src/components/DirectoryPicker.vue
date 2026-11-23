<script setup lang="ts">
import { ref, watch } from 'vue'
import { api } from '../api/client'
import { t } from '../i18n'
import AppModal from './AppModal.vue'
import DirectoryNode from './DirectoryNode.vue'
import Icon from './Icon.vue'

interface Listing {
  path: string
  parent: string
  entries: Array<{ name: string; path: string; hidden?: boolean; directory?: boolean }>
  truncated?: boolean
}

const props = withDefaults(defineProps<{
  modelValue: boolean
  initialPath?: string
  title?: string
  mode?: 'directory' | 'file'
}>(), { initialPath: '', title: '', mode: 'directory' })

const isFileMode = () => props.mode === 'file'

function parentOf(path: string): string {
  const index = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'))
  return index > 0 ? path.slice(0, index) : ''
}

const emit = defineEmits<{
  'update:modelValue': [value: boolean]
  select: [path: string]
}>()

const roots = ref<Array<{ label: string; path: string; kind?: string }>>([])
const listing = ref<Listing | null>(null)
const selectedPath = ref('')
const manualPath = ref('')
const loading = ref(false)
const error = ref('')
const detail = ref('')
const warning = ref('')

function close() {
  emit('update:modelValue', false)
}

async function loadRoots() {
  try {
    const result = await api.get<{ roots: Array<{ label: string; path: string; kind?: string }> }>('/api/v1/fs/roots')
    roots.value = result.roots || []
  } catch {
    roots.value = []
  }
}

async function loadDirectory(path: string): Promise<boolean> {
  loading.value = true
  error.value = ''
  detail.value = ''
  warning.value = ''
  try {
    const fileFlag = isFileMode() ? '&files=true' : ''
    const data = await api.get<Listing>(`/api/v1/fs/list?path=${encodeURIComponent(path)}${fileFlag}`)
    listing.value = data
    selectedPath.value = data.path
    manualPath.value = data.path
    return true
  } catch (err: any) {
    error.value = err?.message || t('读取目录失败')
    detail.value = err?.suggestion || ''
    return false
  } finally {
    loading.value = false
  }
}

async function init() {
  listing.value = null
  selectedPath.value = ''
  manualPath.value = props.initialPath || ''
  error.value = ''
  detail.value = ''
  warning.value = ''
  await loadRoots()
  const start = (props.initialPath || '').trim()
  if (isFileMode() && start) {
    try {
      const file = await api.get<{ path: string }>(`/api/v1/fs/file?path=${encodeURIComponent(start)}`)
      selectedPath.value = file.path
      manualPath.value = file.path
      const ok = await loadDirectory(parentOf(file.path))
      if (ok) selectedPath.value = file.path
    } catch {
      await loadDirectory('')
      warning.value = t('原路径不可用，已从默认位置开始浏览。')
    }
  } else if (start) {
    const ok = await loadDirectory(start)
    if (!ok) {
      error.value = ''
      detail.value = ''
      await loadDirectory('')
      warning.value = t('原路径不可用，已从默认位置开始浏览。')
    }
  } else {
    await loadDirectory('')
  }
}

async function goUp() {
  if (!listing.value?.parent) return
  await loadDirectory(listing.value.parent)
}

async function goToRoot(path: string) {
  await loadDirectory(path)
}

async function confirmSelection() {
  if (!selectedPath.value || loading.value) return
  if (isFileMode()) {
    try {
      const file = await api.get<{ path: string }>(`/api/v1/fs/file?path=${encodeURIComponent(selectedPath.value)}`)
      emit('select', file.path)
    } catch (err: any) {
      error.value = err?.message || t('读取文件失败')
      detail.value = err?.suggestion || ''
      return
    }
  } else {
    emit('select', selectedPath.value)
  }
  close()
}

watch(() => props.modelValue, (open) => {
  if (open) void init()
}, { immediate: true })
</script>

<template>
  <AppModal
    :model-value="modelValue"
    :title="title || (isFileMode() ? t('选择文件') : t('选择目录'))"
    :subtitle="isFileMode() ? t('浏览本机目录树并选择文件；只读取文件路径与大小，不读取文件内容。') : t('浏览本机目录树，选中后点击确认；只读取目录名称，不读取文件内容。')"
    wide
    @update:model-value="emit('update:modelValue', $event)"
  >
    <div class="dir-picker">
      <div class="dir-toolbar">
        <button class="btn btn-sm" :disabled="!listing?.parent || loading" @click="goUp">
          <Icon name="arrow" :size="12" style="transform: rotate(180deg)" /> {{ t('上一级') }}
        </button>
        <input
          v-model="manualPath"
          class="input mono dir-path-input"
          :placeholder="t('输入绝对路径后回车')"
          @keyup.enter="loadDirectory(manualPath)"
        />
        <button class="btn btn-sm" :disabled="loading" @click="loadDirectory(manualPath)">{{ t('前往') }}</button>
      </div>

      <div v-if="roots.length" class="dir-roots">
        <span class="faint tiny">{{ t('位置') }}</span>
        <button v-for="root in roots" :key="root.path" class="dir-chip" @click="goToRoot(root.path)">{{ root.label }}</button>
      </div>

      <div v-if="warning" class="notice warning">{{ warning }}</div>

      <div v-if="error" class="notice danger dir-error-panel">
        <Icon name="warning" :size="14" class="notice-icon" />
        <div style="flex: 1">
          <div>{{ error }}</div>
          <div v-if="detail" class="faint tiny">{{ detail }}</div>
        </div>
        <button class="btn btn-sm" @click="loadDirectory(manualPath)">{{ t('重试') }}</button>
      </div>

      <div class="dir-tree" :class="{ 'is-loading': loading }">
        <div v-if="loading && !listing" class="dir-status"><span class="spinner"></span> {{ t('正在读取目录…') }}</div>
        <template v-else-if="listing">
          <div v-if="!listing.entries.length" class="dir-status">{{ isFileMode() ? t('该目录没有可显示的内容。') : t('该目录没有子目录，可直接确认选择。') }}</div>
          <DirectoryNode
            v-for="entry in listing.entries"
            :key="entry.path"
            :node="entry"
            :depth="0"
            :selected="selectedPath"
            :file-mode="isFileMode()"
            @select="selectedPath = $event"
          />
          <div v-if="listing.truncated" class="notice info">{{ t('目录项过多，仅显示前 2000 个。') }}</div>
        </template>
        <div v-if="loading && listing" class="dir-loading-mask"><span class="spinner"></span></div>
      </div>

      <div class="dir-selected">
        <div class="field-label">{{ isFileMode() ? t('当前选中文件') : t('当前选中目录') }}</div>
        <div class="mono dir-selected-path">{{ selectedPath || t('尚未选择目录') }}</div>
      </div>
    </div>

    <template #footer>
      <button class="btn" @click="close">{{ t('取消') }}</button>
      <button class="btn btn-primary" :disabled="!selectedPath || loading" @click="confirmSelection">
        <Icon name="check" :size="13" /> {{ isFileMode() ? t('确认选择文件') : t('确认选择') }}
      </button>
    </template>
  </AppModal>
</template>
