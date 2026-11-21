<script setup lang="ts">
import { t } from '../i18n'
import { computed, nextTick, ref, watch } from 'vue'
import type { LogEntry } from '../types'
import Icon from './Icon.vue'
import { useAppStore } from '../stores/app'

const props = withDefaults(defineProps<{
  entries: LogEntry[]
  height?: number
  downloadUrl?: string
  title?: string
}>(), { height: 320, downloadUrl: '', title: 'process.log' })
const emit = defineEmits<{ clear: [] }>()
const app = useAppStore()

const search = ref('')
const level = ref('ALL')
const autoScroll = ref(true)
const body = ref<HTMLElement | null>(null)
const levels = ['ALL', 'INFO', 'WARNING', 'ERROR', 'DEBUG']

const filtered = computed(() => {
  const needle = search.value.trim().toLowerCase()
  return props.entries.filter((entry) => {
    if (level.value !== 'ALL' && entry.level !== level.value) return false
    if (needle && !entry.message.toLowerCase().includes(needle)) return false
    return true
  })
})

watch(() => props.entries.length, async () => {
  if (!autoScroll.value) return
  await nextTick()
  if (body.value) body.value.scrollTop = body.value.scrollHeight
})

function toggleAutoScroll() {
  autoScroll.value = !autoScroll.value
  if (autoScroll.value && body.value) body.value.scrollTop = body.value.scrollHeight
}

async function copyLogs() {
  const text = filtered.value.map((item) => `[${item.time}] [${item.level}] ${item.message}`).join('\n')
  try {
    await navigator.clipboard.writeText(text)
    app.toast('success', t('日志已复制'), t('{0} 行', [filtered.value.length]))
  } catch {
    app.toast('error', t('复制失败'), t('浏览器拒绝了剪贴板访问'))
  }
}

function downloadLogs() {
  if (props.downloadUrl) {
    const link = document.createElement('a')
    link.href = props.downloadUrl
    link.click()
    return
  }
  const text = filtered.value.map((item) => `[${item.time}] [${item.level}] ${item.message}`).join('\n')
  const blob = new Blob([text], { type: 'text/plain;charset=utf-8' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = props.title.replace(/\.log$/, '') + '.log'
  link.click()
  URL.revokeObjectURL(url)
}

function shortTime(value: string) {
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return date.toLocaleTimeString('zh-CN', { hour12: false })
}
</script>

<template>
  <div class="console">
    <div class="console-head">
      <span class="status-dot" :class="entries.length ? 'online' : ''"></span>
      <span class="console-title">{{ title }}</span>
      <span class="console-title" style="color: var(--faint); margin-left: 2px">{{ filtered.length }} lines</span>
      <div class="console-head-actions">
        <button class="log-filter" :class="{ active: autoScroll }" :title="t('自动滚动')" @click="toggleAutoScroll">
          {{ autoScroll ? 'FOLLOW' : 'PAUSED' }}
        </button>
      </div>
    </div>
    <div class="log-toolbar">
      <input v-model="search" class="log-search" :placeholder="t('搜索日志 / grep')" />
      <button
        v-for="item in levels"
        :key="item"
        class="log-filter"
        :class="{ active: level === item }"
        @click="level = item"
      >{{ item }}</button>
      <div style="flex: 1"></div>
      <button class="icon-btn" :title="t('复制')" @click="copyLogs"><Icon name="copy" :size="12" /></button>
      <button class="icon-btn" :title="t('下载')" @click="downloadLogs"><Icon name="download" :size="12" /></button>
      <button class="icon-btn" :title="t('清空显示')" @click="emit('clear')"><Icon name="trash" :size="12" /></button>
    </div>
    <div ref="body" class="console-body" :style="{ height: height + 'px' }">
      <div v-if="!filtered.length" class="log-empty">{{ t('尚无匹配日志。转换进程输出会实时显示在这里。') }}</div>
      <div v-for="entry in filtered" :key="entry.seq" class="log-line" :class="entry.level">
        <span class="log-time">{{ shortTime(entry.time) }}</span>
        <span class="log-level">{{ entry.level }}</span>
        <span class="log-message">{{ entry.message }}</span>
      </div>
    </div>
  </div>
</template>
