import type { Envelope, UpdateInfo, UpdateJob } from '../types'
import { t } from '../i18n'

export class ApiError extends Error {
  code: string
  suggestion?: string
  details?: unknown
  status: number

  constructor(code: string, message: string, status: number, suggestion?: string, details?: unknown) {
    super(message)
    this.name = 'ApiError'
    this.code = code
    this.status = status
    this.suggestion = suggestion
    this.details = details
  }
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(path, {
    ...init,
    headers: {
      ...(init.body ? { 'Content-Type': 'application/json' } : {}),
      ...(init.headers || {})
    }
  })
  const text = await response.text()
  let payload: Envelope<T> | null = null
  try {
    payload = text ? JSON.parse(text) : null
  } catch {
    throw new ApiError('INVALID_RESPONSE', t('服务端返回了非 JSON 响应（HTTP {0}）', [response.status]), response.status)
  }
  if (!payload) throw new ApiError('EMPTY_RESPONSE', '服务端返回空响应', response.status)
  if (!response.ok || !payload.ok) {
    const err = payload.error || { code: 'REQUEST_FAILED', message: t('请求失败（HTTP {0}）', [response.status]) }
    throw new ApiError(err.code, err.message, response.status, err.suggestion, err.details)
  }
  return payload.data
}

export const api = {
  get<T>(path: string): Promise<T> {
    return request<T>(path)
  },
  post<T>(path: string, body?: unknown): Promise<T> {
    return request<T>(path, { method: 'POST', body: JSON.stringify(body === undefined ? {} : body) })
  },
  put<T>(path: string, body?: unknown): Promise<T> {
    return request<T>(path, { method: 'PUT', body: JSON.stringify(body ?? {}) })
  },
  patch<T>(path: string, body?: unknown): Promise<T> {
    return request<T>(path, { method: 'PATCH', body: JSON.stringify(body ?? {}) })
  },
  delete<T>(path: string): Promise<T> {
    return request<T>(path, { method: 'DELETE' })
  }
}

export const updatesApi = {
  check: () => api.get<UpdateInfo>('/api/v1/updates/check'),
  download: () => api.post<{ jobId: string; message: string }>('/api/v1/updates/download'),
  job: (id: string) => api.get<UpdateJob>(`/api/v1/updates/job/${encodeURIComponent(id)}`),
  apply: (jobId: string) => api.post<{ message: string; scriptPath: string }>('/api/v1/updates/apply', { jobId })
}

export function humanBytes(bytes: number | null | undefined): string {
  if (bytes === null || bytes === undefined || Number.isNaN(bytes)) return '—'
  if (bytes === 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit++
  }
  return `${value.toFixed(unit === 0 ? 0 : value >= 100 ? 1 : 2)} ${units[unit]}`
}

export function humanNumber(value: number | null | undefined): string {
  if (!value && value !== 0) return '—'
  if (value >= 1e12) return `${(value / 1e12).toFixed(2)}T`
  if (value >= 1e9) return `${(value / 1e9).toFixed(2)}B`
  if (value >= 1e6) return `${(value / 1e6).toFixed(2)}M`
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}K`
  return `${value}`
}

export function formatDuration(ms: number | null | undefined): string {
  if (!ms || ms < 0) return '—'
  const seconds = Math.floor(ms / 1000)
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = seconds % 60
  if (h) return `${h}h ${m}m`
  if (m) return `${m}m ${s}s`
  return `${s}s`
}

export function formatTime(value: number | string | null | undefined): string {
  if (!value) return '—'
  const date = typeof value === 'number' ? new Date(value) : new Date(value)
  if (Number.isNaN(date.getTime())) return String(value)
  return date.toLocaleString('zh-CN', { hour12: false })
}

export function relativeTime(value: number | string | null | undefined): string {
  if (!value) return '—'
  const date = typeof value === 'number' ? new Date(value) : new Date(value)
  const diff = Date.now() - date.getTime()
  if (diff < 60_000) return t('刚刚')
  if (diff < 3_600_000) return t('{0} 分钟前', [Math.floor(diff / 60_000)])
  if (diff < 86_400_000) return t('{0} 小时前', [Math.floor(diff / 3_600_000)])
  return t('{0} 天前', [Math.floor(diff / 86_400_000)])
}

export function statusLabel(status: string): string {
  const map: Record<string, string> = {
    QUEUED: '等待中',
    RUNNING: '运行中',
    PAUSED: '已暂停',
    SUCCEEDED: '已完成',
    FAILED: '失败',
    CANCELED: '已取消',
    READY: '就绪',
    INCOMPLETE: '不完整',
    pending: '等待中',
    resolving: '解析中',
    downloading: '下载中',
    paused: '已暂停',
    retrying: '重试中',
    verifying: '校验中',
    completed: '已完成',
    failed: '失败',
    cancelled: '已取消'
  }
  return map[status] ? t(map[status]) : status
}

export function statusTone(status: string): string {
  if (status === 'SUCCEEDED' || status === 'READY' || status === 'completed') return 'success'
  if (status === 'FAILED' || status === 'failed') return 'danger'
  if (status === 'RUNNING' || status === 'downloading') return 'active'
  if (status === 'PAUSED' || status === 'INCOMPLETE' || status === 'paused' || status === 'retrying') return 'warning'
  if (status === 'CANCELED' || status === 'cancelled') return 'muted'
  return 'info'
}
