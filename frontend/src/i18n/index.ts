import { ref } from 'vue'
import en from './en-US'

export type Locale = 'zh-CN' | 'en-US'

const STORAGE_KEY = 'modelforge.locale'

// The Chinese source text is the key. Missing translations fall back to the source
// string, so a partially translated build never shows raw ids — but the dictionary
// is kept complete for every user-facing string.
const dictionaries: Record<string, Record<string, string>> = {
  'en-US': en
}

function detectLocale(): Locale {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved === 'en-US' || saved === 'zh-CN') return saved
  } catch {
    /* localStorage can be unavailable in private modes */
  }
  const nav = typeof navigator !== 'undefined' ? navigator.language : 'zh-CN'
  return nav.toLowerCase().startsWith('zh') ? 'zh-CN' : 'en-US'
}

export const locale = ref<Locale>(detectLocale())

export const locales: Array<{ value: Locale; label: string }> = [
  { value: 'zh-CN', label: '简体中文' },
  { value: 'en-US', label: 'English' }
]

export function t(
  key: string,
  params?: Record<string, string | number> | Array<string | number>
): string {
  const dict = dictionaries[locale.value]
  let text = (dict && dict[key]) || key
  if (Array.isArray(params)) {
    params.forEach((value, index) => {
      text = text.split(`{${index}}`).join(String(value))
    })
  } else if (params) {
    for (const name of Object.keys(params)) {
      text = text.split(`{${name}}`).join(String(params[name]))
    }
  }
  return text
}

export function applyLocale(next: Locale | string | undefined | null): void {
  const value: Locale = next === 'en-US' ? 'en-US' : 'zh-CN'
  locale.value = value
  try {
    localStorage.setItem(STORAGE_KEY, value)
  } catch {
    /* ignore */
  }
  if (typeof document !== 'undefined') {
    document.documentElement.lang = value
  }
}

// Applies the cached value as early as possible to avoid a language flash.
applyLocale(locale.value)
