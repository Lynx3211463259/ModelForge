<script setup lang="ts">
import { ref } from 'vue'
import { api } from '../api/client'
import { t } from '../i18n'
import Icon from './Icon.vue'

defineOptions({ name: 'DirectoryNode' })

const props = defineProps<{
  node: { name: string; path: string; hidden?: boolean }
  depth: number
  selected: string
}>()

const emit = defineEmits<{
  (event: 'select', path: string): void
}>()

const expanded = ref(false)
const loaded = ref(false)
const loading = ref(false)
const error = ref('')
const detail = ref('')
const children = ref<Array<{ name: string; path: string; hidden?: boolean }>>([])

async function load() {
  loading.value = true
  error.value = ''
  detail.value = ''
  try {
    const result = await api.get<{ entries: Array<{ name: string; path: string; hidden?: boolean }> }>(
      `/api/v1/fs/list?path=${encodeURIComponent(props.node.path)}`
    )
    children.value = result.entries || []
    loaded.value = true
  } catch (err: any) {
    error.value = err?.message || t('读取目录失败')
    detail.value = err?.suggestion || ''
  } finally {
    loading.value = false
  }
}

function toggle() {
  expanded.value = !expanded.value
  if (expanded.value && !loaded.value && !loading.value) void load()
}
</script>

<template>
  <div class="dir-node">
    <div
      class="dir-row"
      :class="{ selected: selected === node.path, dim: node.hidden }"
      :style="{ paddingLeft: `${depth * 14 + 6}px` }"
      @click="emit('select', node.path)"
      @dblclick="toggle"
    >
      <button class="dir-caret-btn" :aria-label="expanded ? t('折叠') : t('展开')" @click.stop="toggle">
        <span class="dir-caret" :class="{ open: expanded }"></span>
      </button>
      <Icon name="folder" :size="13" />
      <span class="dir-name">{{ node.name }}</span>
    </div>
    <div v-if="expanded" class="dir-children">
      <div v-if="loading" class="dir-status"><span class="spinner"></span> {{ t('正在读取…') }}</div>
      <div v-else-if="error" class="dir-error">
        <div>{{ error }}<div v-if="detail" class="dir-error-detail">{{ detail }}</div></div>
        <button class="btn btn-sm" @click.stop="load">{{ t('重试') }}</button>
      </div>
      <div v-else-if="!children.length" class="dir-status">{{ t('没有子目录') }}</div>
      <DirectoryNode
        v-for="child in children"
        :key="child.path"
        :node="child"
        :depth="depth + 1"
        :selected="selected"
        @select="emit('select', $event)"
      />
    </div>
  </div>
</template>
