<script setup lang="ts">
import { applyLocale, t } from '../i18n'
import { applyTheme } from '../theme'
import { onMounted, ref } from 'vue'
import { api } from '../api/client'
import type { Settings } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'

const app = useAppStore()
const form = ref<Settings | null>(null)
const loading = ref(true)
const saving = ref(false)
const testing = ref(false)
const checks = ref<any[]>([])
const restartRequired = ref(false)
const importInput = ref<HTMLInputElement | null>(null)

async function load() {
  loading.value = true
  try {
    form.value = await api.get<Settings>('/api/v1/settings')
  } catch (error) {
    app.fromError(error, t('加载设置失败'))
  } finally {
    loading.value = false
  }
}

async function save() {
  if (!form.value) return
  saving.value = true
  try {
    const result = await api.put<{ settings: Settings; restartRequired: boolean }>('/api/v1/settings', form.value)
    form.value = result.settings
    app.settings = result.settings
    restartRequired.value = result.restartRequired
    app.toast('success', t('设置已保存'), result.restartRequired ? t('端口变更将在重启后生效') : t('新任务会使用最新配置'))
  } catch (error) {
    app.fromError(error, t('保存设置失败'))
  } finally {
    saving.value = false
  }
}

function onThemeChange(value: string) {
  applyTheme(value)
  if (app.settings) app.settings.theme = value
}

function onLanguageChange(value: string) {
  applyLocale(value)
  if (app.settings) app.settings.language = value
}

async function testToolchain() {
  testing.value = true
  checks.value = []
  try {
    const result = await api.post<{ checks: any[] }>('/api/v1/settings/test')
    checks.value = result.checks
    await app.refreshCapabilities(false)
    const failed = result.checks.filter((item: any) => !item.ok).length
    app.toast(failed ? 'warning' : 'success',
      failed ? t('{0} 项检查未通过', [failed]) : t('工具链检查全部通过'))
  } catch (error) {
    app.fromError(error, t('工具链检测失败'))
  } finally {
    testing.value = false
  }
}

async function importSettings(event: Event) {
  const file = (event.target as HTMLInputElement).files?.[0]
  if (!file) return
  try {
    const payload = JSON.parse(await file.text())
    const result = await api.post<Settings>('/api/v1/settings/import', payload)
    form.value = result
    app.settings = result
    app.toast('success', t('设置已导入'))
  } catch (error) {
    app.fromError(error, t('导入设置失败'))
  } finally {
    ;(event.target as HTMLInputElement).value = ''
  }
}

onMounted(async () => { await app.init(); await load() })
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('系统设置') }}</h1>
        <p class="page-subtitle">{{ t('工具链和目录配置保存在本地 SQLite；修改后由同一核心逻辑供 Web UI 与 CLI 使用。') }}</p>
      </div>
      <div class="page-actions">
        <button class="btn" @click="testToolchain" :disabled="testing"><span v-if="testing" class="spinner"></span><Icon v-else name="activity" :size="13" /> {{ t('检测工具链') }}</button>
        <button class="btn" @click="importInput?.click()"><Icon name="upload" :size="13" /> {{ t('导入') }}</button>
        <a class="btn" href="/api/v1/settings/export"><Icon name="download" :size="13" /> {{ t('导出') }}</a>
        <button class="btn btn-primary" :disabled="saving || !form" @click="save"><span v-if="saving" class="spinner"></span><Icon v-else name="save" :size="13" /> {{ t('保存设置') }}</button>
        <input ref="importInput" type="file" accept="application/json" style="display: none" @change="importSettings" />
      </div>
    </div>

    <div v-if="restartRequired" class="notice warning" style="margin-bottom: 12px">
      <Icon name="warning" :size="14" class="notice-icon" />
      <div>{{ t('端口已变更。关闭并重新启动 ModelForge 后，Web 服务才会监听新端口。') }}</div>
    </div>

    <div v-if="loading || !form" class="loading-panel"><span class="spinner"></span> {{ t('正在加载配置…') }}</div>
    <div v-else class="split-panel">
      <div class="card">
        <div class="card-header"><div><div class="card-title">{{ t('转换工具链') }}</div><div class="card-subtitle">{{ t('真实路径，不通过 Shell 拼接') }}</div></div><span class="badge" :class="app.capabilities?.conversionReady ? 'success' : 'danger'">{{ t('{0}', [app.capabilities?.conversionReady ? t('就绪') : t('未就绪')]) }}</span></div>
        <div class="card-body">
          <div class="setting-group">
            <div class="setting-group-title">{{ t('Python 与转换脚本') }}</div>
            <div class="setting-group-copy">{{ t('Python 只作为 llama.cpp 转换工具的运行时依赖，不承载 ModelForge 业务逻辑。') }}</div>
            <div class="field"><label class="field-label">{{ t('Python 可执行文件') }}</label><input v-model="form.pythonPath" class="input mono" placeholder="python" /><div class="field-hint">{{ t('可使用绝对路径或 PATH 中的命令名。') }}</div></div>
            <div class="field"><label class="field-label">convert_hf_to_gguf.py</label><input v-model="form.convertScript" class="input mono" /></div>
          </div>

          <div class="setting-group">
            <div class="setting-group-title">{{ t('llama.cpp 工具') }}</div>
            <div class="setting-group-copy">{{ t('量化类型从 llama-quantize --help 动态解析；缺失时不会展示虚假的可执行类型。') }}</div>
            <div class="notice warning" style="margin-bottom: 10px">
              <Icon name="info" :size="13" class="notice-icon" />
              <div>
                {{ t('随包分发的 llama.cpp 为官方 release 原样解压产物。') }}
                <router-link to="/runtime" style="color: var(--accent); text-decoration: underline">{{ t('打开“运行引擎”') }}</router-link> {{ t('可查看官方版本、检查更新并按当前平台下载官方资源。') }}
              </div>
            </div>
            <div class="field"><label class="field-label">{{ t('llama.cpp 目录') }}</label><input v-model="form.llamaRoot" class="input mono" /><div class="field-hint">{{ t('官方 zip 保存在 downloads/，原样解压到 releases/<资产名>/。') }}</div></div>
            <div class="field"><label class="field-label">llama-quantize</label><input v-model="form.quantizePath" class="input mono" /></div>
            <div class="field"><label class="field-label">llama-cli</label><input v-model="form.llamaCliPath" class="input mono" /></div>
            <div class="field"><label class="field-label">llama-server</label><input v-model="form.llamaServerPath" class="input mono" /></div>
            <div class="field"><label class="field-label">llama-bench</label><input v-model="form.llamaBenchPath" class="input mono" /></div>
            <div class="field"><label class="field-label">llama-perplexity</label><input v-model="form.llamaPplPath" class="input mono" /></div>
            <div class="field">
              <label class="field-label">{{ t('困惑度评测文本') }}</label>
              <input v-model="form.pplDataset" class="input mono" :placeholder="t('留空则使用 data/quality 下的默认样本')" />
              <div class="field-hint">{{ t('标准对比请使用 wikitext-2 的 wiki.test.raw；内置样本只适合同一模型内部的相对比较。') }}</div>
            </div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header"><div><div class="card-title">{{ t('目录与运行') }}</div><div class="card-subtitle">{{ t('推荐将大文件输出到独立磁盘') }}</div></div></div>
        <div class="card-body">
          <div class="setting-group">
            <div class="setting-group-title">{{ t('工作目录') }}</div>
            <div class="field"><label class="field-label">{{ t('默认模型目录') }}</label><input v-model="form.defaultModelDir" class="input mono" /></div>
            <div class="field"><label class="field-label">{{ t('默认输出目录') }}</label><input v-model="form.defaultOutputDir" class="input mono" /><div class="field-hint">{{ t('转换前仍会检查该目录的可用磁盘空间与写权限。') }}</div></div>
            <div class="field"><label class="field-label">{{ t('临时目录') }}</label><input v-model="form.tempDir" class="input mono" /></div>
            <div class="kv"><span>{{ t('数据库与日志目录') }}</span><span>{{ form.dataDir }}</span></div>
          </div>

          <div class="setting-group">
            <div class="setting-group-title">{{ t('调度') }}</div>
            <div class="field-row">
              <div class="field"><label class="field-label">{{ t('CPU 线程数') }}</label><input v-model.number="form.threads" type="number" min="1" max="512" class="input" /></div>
              <div class="field"><label class="field-label">{{ t('最大并发任务') }}</label><input v-model.number="form.maxConcurrent" type="number" min="1" max="8" class="input" /></div>
            </div>
            <label class="check"><input v-model="form.autoOpenBrowser" type="checkbox" /><span>{{ t('启动时自动打开浏览器') }}<small>{{ t('仅对 serve 模式生效') }}</small></span></label>
          </div>

          <div class="setting-group">
            <div class="setting-group-title">{{ t('Web 服务') }}</div>
            <div class="field-row">
              <div class="field"><label class="field-label">{{ t('监听地址') }}</label><input v-model="form.bindAddress" class="input mono" disabled /><div class="field-hint">{{ t('当前构建固定监听本机回环地址。') }}</div></div>
              <div class="field"><label class="field-label">{{ t('端口') }}</label><input v-model.number="form.port" type="number" min="1024" max="65535" class="input" /></div>
            </div>
            <div class="field-row" style="margin-top: 8px">
              <div class="field">
                <label class="field-label">{{ t('主题') }}</label>
                <select v-model="form.theme" class="select" @change="onThemeChange(form.theme)">
                  <option value="dark">{{ t('深色') }}</option>
                  <option value="light">{{ t('浅色') }}</option>
                  <option value="system">{{ t('跟随系统') }}</option>
                </select>
                <div class="field-hint">{{ t('浅色主题只切换配色，不改变布局与功能。') }}</div>
              </div>
              <div class="field">
                <label class="field-label">{{ t('语言') }}</label>
                <select v-model="form.language" class="select" @change="onLanguageChange(form.language)">
                  <option value="zh-CN">简体中文</option>
                  <option value="en-US">English</option>
                </select>
                <div class="field-hint">{{ t('切换后立即生效，保存设置后持久化到服务端。') }}</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <div v-if="checks.length" class="card" style="margin-top: 14px">
      <div class="card-header"><div><div class="card-title">{{ t('最近一次工具链检查') }}</div><div class="card-subtitle">{{ t('真实执行 --version 与依赖导入测试') }}</div></div><span class="tag">{{ checks.filter(c => c.ok).length }} / {{ checks.length }}</span></div>
      <div class="card-body">
        <div class="split-panel" style="grid-template-columns: repeat(2, 1fr)">
          <div v-for="check in checks" :key="check.id" class="check-item">
            <span class="check-mark" :class="check.ok ? 'pass' : 'fail'"><Icon :name="check.ok ? 'check' : 'close'" :size="9" :stroke="2" /></span>
            <div style="min-width: 0"><div class="check-title">{{ check.id }}</div><div class="check-detail">{{ check.detail || '—' }}</div></div>
          </div>
        </div>
      </div>
    </div>

  </div>
</template>
