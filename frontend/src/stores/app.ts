import { defineStore } from 'pinia'
import { api, ApiError } from '../api/client'
import type { Capabilities, Settings } from '../types'
import { t } from '../i18n'

export interface Toast {
  id: number
  title: string
  message?: string
  tone: 'info' | 'success' | 'error' | 'warning'
  suggestion?: string
  timeout?: number
}

let toastId = 1

export const useAppStore = defineStore('app', {
  state: () => ({
    settings: null as Settings | null,
    capabilities: null as Capabilities | null,
    health: null as Record<string, any> | null,
    initialized: false,
    loading: false,
    error: '',
    realtimeStatus: 'connecting' as 'connecting' | 'online' | 'offline' | 'sse',
    realtimeInfo: null as Record<string, any> | null,
    lastEventAt: 0,
    toasts: [] as Toast[],
    sidebarOpen: false,
    _eventSource: null as EventSource | null
  }),
  actions: {
    toast(tone: Toast['tone'], title: string, message = '', suggestion = '', timeout = 4200) {
      const item: Toast = { id: toastId++, tone, title, message, suggestion, timeout }
      this.toasts.push(item)
      if (timeout > 0) {
        window.setTimeout(() => this.dismissToast(item.id), timeout)
      }
      return item.id
    },
    dismissToast(id: number) {
      this.toasts = this.toasts.filter((item) => item.id !== id)
    },
    fromError(error: unknown, title = t('操作失败')) {
      if (error instanceof ApiError) {
        this.toast('error', title, error.message, error.suggestion || '', 7800)
      } else if (error instanceof Error) {
        this.toast('error', title, error.message, '', 6500)
      } else {
        this.toast('error', title, String(error), '', 6500)
      }
    },
    async init(force = false) {
      if (this.initialized && !force) return
      this.loading = true
      this.error = ''
      try {
        const [settings, capabilities, health] = await Promise.all([
          api.get<Settings>('/api/v1/settings'),
          api.get<Capabilities>('/api/v1/capabilities'),
          api.get<Record<string, any>>('/api/v1/health')
        ])
        this.settings = settings
        this.capabilities = capabilities
        this.health = health
        this.initialized = true
        if (!this.health?.websocketRunning) {
          this.realtimeStatus = 'sse'
          this.startSse()
        } else {
          this.connectWebSocket()
        }
      } catch (error) {
        this.error = error instanceof Error ? error.message : String(error)
        this.realtimeStatus = 'offline'
        this.fromError(error, t('无法连接 ModelForge 后端'))
      } finally {
        this.loading = false
      }
    },
    async refreshCapabilities(force = true) {
      try {
        this.capabilities = await api.get<Capabilities>(`/api/v1/capabilities${force ? '?refresh=1' : ''}`)
      } catch (error) {
        this.fromError(error, t('能力检测失败'))
      }
    },
    async refreshSettings() {
      this.settings = await api.get<Settings>('/api/v1/settings')
      return this.settings
    },
    connectWebSocket() {
      if (!this.health?.websocketRunning || !this.health?.websocketPort) {
        this.realtimeStatus = 'sse'
        this.startSse()
        return
      }
      const protocol = location.protocol === 'https:' ? 'wss' : 'ws'
      const url = `${protocol}://${location.hostname}:${this.health.websocketPort}/ws`
      try {
        const socket = new WebSocket(url)
        socket.onopen = () => {
          this.realtimeStatus = 'online'
          this.closeSse()
        }
        socket.onmessage = (event) => {
          try {
            this.dispatchEvent(JSON.parse(event.data))
          } catch {
            // Ignore malformed frames from non-ModelForge clients.
          }
        }
        socket.onerror = () => {
          this.realtimeStatus = 'sse'
        }
        socket.onclose = () => {
          if (this.realtimeStatus === 'online') this.realtimeStatus = 'offline'
          this.startSse()
          window.setTimeout(() => {
            if (this.realtimeStatus !== 'online') this.connectWebSocket()
          }, 5000)
        }
      } catch {
        this.realtimeStatus = 'sse'
        this.startSse()
      }
    },
    startSse() {
      if (this._eventSource) return
      try {
        const source = new EventSource('/api/v1/events')
        this._eventSource = source
        source.onopen = () => {
          if (this.realtimeStatus !== 'online') this.realtimeStatus = 'sse'
        }
        source.onmessage = (event) => {
          try {
            this.dispatchEvent(JSON.parse(event.data))
          } catch {
            // Ignore keep-alive parse errors.
          }
        }
        source.onerror = () => {
          if (this.realtimeStatus !== 'online') this.realtimeStatus = 'offline'
        }
      } catch {
        this.realtimeStatus = 'offline'
      }
    },
    closeSse() {
      if (this._eventSource) {
        this._eventSource.close()
        this._eventSource = null
      }
    },
    dispatchEvent(payload: Record<string, any>) {
      this.lastEventAt = Date.now()
      window.dispatchEvent(new CustomEvent('mf:event', { detail: payload }))
    }
  },
  getters: {
    engineReady(state): boolean {
      return Boolean(state.capabilities?.conversionReady)
    },
    wsLabel(state): string {
      return state.realtimeStatus === 'online' ? t('实时通道已连接')
        : state.realtimeStatus === 'sse' ? t('SSE 实时通道')
          : state.realtimeStatus === 'connecting' ? t('正在连接') : t('实时通道离线')
    }
  }
})
