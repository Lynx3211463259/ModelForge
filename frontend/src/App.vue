<script setup lang="ts">
import { applyLocale, locale, t } from './i18n'
import { applyTheme, resolveTheme, themeMode, type ThemeMode } from './theme'
import { api } from './api/client'
import { computed, onMounted, watch } from 'vue'
import { useRoute } from 'vue-router'
import { useAppStore } from './stores/app'
import Icon from './components/Icon.vue'

const route = useRoute()
const app = useAppStore()

const navGroups = computed(() => [
  {
    label: 'Workspace',
    items: [
      { to: '/', icon: 'dashboard', label: t('总览') },
      { to: '/models', icon: 'models', label: t('模型管理') },
      { to: '/convert', icon: 'convert', label: t('模型转换') },
      { to: '/tasks', icon: 'tasks', label: t('批量任务') }
    ]
  },
  {
    label: 'Intelligence',
    items: [
      { to: '/lab', icon: 'lab', label: t('量化实验室') },
      { to: '/presets', icon: 'layers', label: t('配置预设') },
      { to: '/analyzer', icon: 'analyzer', label: t('模型分析') },
      { to: '/tester', icon: 'tester', label: t('模型测试') },
      { to: '/gguf', icon: 'gguf', label: t('GGUF 编辑器') }
    ]
  },
  {
    label: 'Platform',
    items: [
      { to: '/workflows', icon: 'workflows', label: t('工作流') },
      { to: '/history', icon: 'history', label: t('转换历史') },
      { to: '/downloads', icon: 'downloads', label: t('下载管理') },
      { to: '/watch', icon: 'folder', label: t('文件夹监控') },
      { to: '/runtime', icon: 'cpu', label: t('运行引擎') },
      { to: '/plugins', icon: 'plugins', label: t('插件管理') },
      { to: '/settings', icon: 'settings', label: t('系统设置') }
    ]
  }
])

const resolvedTheme = computed(() => resolveTheme(themeMode.value))
const title = computed(() => t((route.meta.title as string) || 'ModelForge'))
const subtitle = computed(() => t((route.meta.subtitle as string) || ''))
const portFallback = computed(() => Boolean(app.health?.portFallback))

const realtimeTone = computed(() => {
  if (app.realtimeStatus === 'online' || app.realtimeStatus === 'sse') return 'online'
  if (app.realtimeStatus === 'connecting') return 'warn'
  return 'offline'
})

watch(
  () => app.settings?.theme,
  (value) => applyTheme((value as ThemeMode) || null),
  { immediate: true }
)
watch(
  () => app.settings?.language,
  (value) => { if (value) applyLocale(value) },
  { immediate: true }
)

// Quick toggle in the top bar: flips between the two explicit themes. The
// three-state control (including "follow system") stays in Settings.
function toggleTheme() {
  const next: ThemeMode = resolvedTheme.value === 'light' ? 'dark' : 'light'
  applyTheme(next)
  if (app.settings) app.settings.theme = next
  api.put('/api/v1/settings', { theme: next }).catch(() => {
    // The theme already applied locally; a failed persist only affects other clients.
  })
}

// Keep the document title in sync with the active locale as well as navigation.
watch([title, () => route.fullPath], () => {
  document.title = `${title.value} · ModelForge`
})

onMounted(() => app.init())
watch(() => route.fullPath, () => { app.sidebarOpen = false })
</script>

<template>
  <div class="app-shell">
    <aside class="sidebar" :class="{ open: app.sidebarOpen }">
      <div class="brand">
        <div class="brand-mark">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none">
            <path d="M12 2.8l8.2 4.7v9.2L12 21.2 3.8 16.7V7.5L12 2.8z" stroke="currentColor" stroke-width="1.4" />
            <path d="M8 10.2l4-2.3 4 2.3v4.2l-4 2.3-4-2.3v-4.2z" stroke="currentColor" stroke-width="1.4" />
            <path d="M12 12v4.3M8 10.2l4 1.8 4-1.8" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" />
          </svg>
        </div>
        <div>
          <div class="brand-name">ModelForge</div>
          <div class="brand-sub">Local model foundry</div>
        </div>
      </div>

      <div class="nav-scroll">
        <template v-for="group in navGroups" :key="group.label">
          <div class="nav-group-label">{{ group.label }}</div>
          <router-link v-for="item in group.items" :key="item.to" :to="item.to" class="nav-item">
            <Icon :name="item.icon" class="nav-icon" />
            <span>{{ item.label }}</span>
          </router-link>
        </template>
      </div>

      <div class="sidebar-footer">
        <div class="engine-status">
          <div class="engine-row">
            <span class="status-dot" :class="app.engineReady ? 'online' : 'warn'"></span>
            <span>{{ t('{0}', [app.engineReady ? t('转换引擎就绪') : t('转换引擎待配置')]) }}</span>
          </div>
          <div class="engine-row">
            <span class="status-dot" :class="realtimeTone"></span>
            <span>{{ app.wsLabel }}</span>
          </div>
          <div class="engine-row" style="margin-top: 8px; color: var(--faint); font-family: var(--font-mono); font-size: 9px">
            v{{ app.health?.version || '0.1.0' }} · LOCAL ONLY
          </div>
        </div>
      </div>
    </aside>

    <div v-if="app.sidebarOpen" class="modal-backdrop" style="z-index: 35" @click="app.sidebarOpen = false"></div>

    <div class="main-wrap">
      <header class="topbar">
        <button class="top-link mobile-menu" :title="t('打开导航')" @click="app.sidebarOpen = !app.sidebarOpen">
          <Icon name="menu" :size="16" />
        </button>
        <div class="crumbs">
          <span class="crumb-kicker">WORKSPACE /</span>
          <span class="crumb-title">{{ title }}</span>
        </div>
        <div class="topbar-spacer"></div>
        <div class="realtime-pill" :title="title + ' · ' + subtitle">
          <span class="status-dot" :class="realtimeTone"></span>
          <span>{{ app.wsLabel }}</span>
        </div>
        <button
          class="top-link theme-toggle"
          :title="resolvedTheme === 'light' ? t('切换到深色模式') : t('切换到浅色模式')"
          :aria-label="resolvedTheme === 'light' ? t('切换到深色模式') : t('切换到浅色模式')"
          @click="toggleTheme"
        >
          <Icon :name="resolvedTheme === 'light' ? 'moon' : 'sun'" :size="15" />
        </button>
        <router-link to="/settings" class="top-link" :title="t('系统设置')">
          <Icon name="settings" :size="15" />
        </router-link>
      </header>

      <main class="page">
        <div v-if="portFallback" class="notice warning port-fallback-banner" role="status">
          <Icon name="warning" :size="15" class="notice-icon" />
          <div>
            <strong>{{ t('端口已临时切换') }}</strong>
            <div>{{ t('设置端口 {0} 本次不可用，当前实际端口为 {1}。设置未被修改，下次启动仍会尝试 {0}。', [app.health?.configuredPort, app.health?.port]) }}</div>
            <div v-if="app.health?.portFallbackReason">{{ t('原因：{0}', [app.health.portFallbackReason]) }}</div>
          </div>
        </div>
        <router-view v-slot="{ Component }">
          <transition name="page" mode="out-in">
            <component :is="Component" :key="locale + route.fullPath" />
          </transition>
        </router-view>
      </main>
    </div>

    <div class="toast-stack">
      <div v-for="toast in app.toasts" :key="toast.id" class="toast" :class="toast.tone">
        <div class="toast-mark"></div>
        <div style="flex: 1; min-width: 0">
          <div class="toast-title">{{ toast.title }}</div>
          <div v-if="toast.message" class="toast-message">{{ toast.message }}</div>
          <div v-if="toast.suggestion" class="toast-suggestion">{{ toast.suggestion }}</div>
        </div>
        <button class="icon-btn toast-close" @click="app.dismissToast(toast.id)">
          <Icon name="close" :size="12" />
        </button>
      </div>
    </div>
  </div>
</template>
