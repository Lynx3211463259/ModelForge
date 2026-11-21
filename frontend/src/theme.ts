import { ref } from 'vue'

export type ThemeMode = 'dark' | 'light' | 'system'

const STORAGE_KEY = 'modelforge.theme'

function detectMode(): ThemeMode {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved === 'dark' || saved === 'light' || saved === 'system') return saved
  } catch {
    /* ignore */
  }
  return 'dark'
}

export const themeMode = ref<ThemeMode>(detectMode())
export const systemPrefersLight = ref(false)

if (typeof window !== 'undefined' && window.matchMedia) {
  const query = window.matchMedia('(prefers-color-scheme: light)')
  systemPrefersLight.value = query.matches
  const listener = (event: MediaQueryListEvent) => {
    systemPrefersLight.value = event.matches
    if (themeMode.value === 'system') applyTheme('system')
  }
  if (query.addEventListener) query.addEventListener('change', listener)
  else if (query.addListener) query.addListener(listener)
}

export function resolveTheme(mode: ThemeMode): 'dark' | 'light' {
  if (mode === 'system') return systemPrefersLight.value ? 'light' : 'dark'
  return mode
}

export function applyTheme(mode: ThemeMode | string | undefined | null): void {
  const value: ThemeMode = mode === 'light' || mode === 'system' ? mode : 'dark'
  themeMode.value = value
  const resolved = resolveTheme(value)
  if (typeof document !== 'undefined') {
    document.documentElement.dataset.theme = resolved
  }
  try {
    localStorage.setItem(STORAGE_KEY, value)
  } catch {
    /* ignore */
  }
}

applyTheme(themeMode.value)

// Charts cannot read CSS variables directly, so resolve the semantic colors at
// render time. Call this inside a computed that also reads `themeMode`, which
// makes the chart re-render when the theme changes.
export function chartPalette() {
  const fallback = {
    text: '#b7c3d3', muted: '#748196', faint: '#526074', line: '#1e2937',
    lineSoft: '#17212d', lineStrong: '#2b3a4d', panel: '#0e141d',
    accent: '#34d6c4', blue: '#5b95ff', green: '#65d69a', amber: '#f0b45a', red: '#f07178'
  }
  if (typeof window === 'undefined') return fallback
  const style = window.getComputedStyle(document.documentElement)
  const read = (name: string, key: keyof typeof fallback) => style.getPropertyValue(name).trim() || fallback[key]
  return {
    text: read('--text-soft', 'text'), muted: read('--muted', 'muted'), faint: read('--faint', 'faint'),
    line: read('--line', 'line'), lineSoft: read('--line-soft', 'lineSoft'),
    lineStrong: read('--line-strong', 'lineStrong'), panel: read('--panel', 'panel'),
    accent: read('--accent', 'accent'), blue: read('--blue', 'blue'), green: read('--green', 'green'),
    amber: read('--amber', 'amber'), red: read('--red', 'red')
  }
}
