<script setup lang="ts">
import { applyLocale, t } from '../i18n'
import { applyTheme } from '../theme'
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, formatTime, humanBytes, updatesApi } from '../api/client'
import type { Settings, UpdateInfo, UpdateJob } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import AppModal from '../components/AppModal.vue'
import DirectoryPicker from '../components/DirectoryPicker.vue'
import ProgressBar from '../components/ProgressBar.vue'

const app = useAppStore()
const form = ref<Settings | null>(null)
const loading = ref(true)
const saving = ref(false)
const testing = ref(false)
const checks = ref<any[]>([])
const restartRequired = ref(false)
const importInput = ref<HTMLInputElement | null>(null)

const updateInfo = ref<UpdateInfo | null>(null)
const updateJob = ref<UpdateJob | null>(null)
const checkingUpdate = ref(false)
const downloadingUpdate = ref(false)
const applyingUpdate = ref(false)
const updateModalOpen = ref(false)
let updatePoll: number | undefined
let versionPoll: number | undefined

const dirPickerOpen = ref(false)
const dirPickerInitial = ref('')
const dirPickerTarget = ref<'' | 'llamaRoot' | 'defaultModelDir' | 'defaultOutputDir' | 'tempDir'>('')

function openDirPicker(target: typeof dirPickerTarget.value) {
  if (!form.value || !target) return
  dirPickerTarget.value = target
  dirPickerInitial.value = String((form.value as any)[target] || '')
  dirPickerOpen.value = true
}

function onDirSelected(path: string) {
  if (form.value && dirPickerTarget.value) (form.value as any)[dirPickerTarget.value] = path
  app.toast('success', t('已选择目录'), path)
}

// The running executable is authoritative: /api/v1/health returns the version
// compiled into it. Fall back to the last GitHub check only when health is
// temporarily unavailable.
const currentVersion = computed(() => app.health?.version || updateInfo.value?.currentVersion || '')

async function refreshVersion() {
  try { await app.refreshHealth() } catch { /* keep the last known version */ }
}

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

async function onLanguageChange(value: string) {
  const next = value === 'en-US' ? 'en-US' : 'zh-CN'
  if (form.value) form.value.language = next
  applyLocale(next)
  if (app.settings) app.settings.language = next
  try {
    await api.put('/api/v1/settings', { language: next })
  } catch (error) {
    app.fromError(error, t('语言设置保存失败'))
  }
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

async function onUpdateChannelChange(value: string) {
  if (!form.value) return
  form.value.updateChannel = value
  updateInfo.value = null
  updateJob.value = null
  try {
    const result = await api.put<{ settings: Settings; restartRequired: boolean }>('/api/v1/settings', { updateChannel: value })
    form.value = result.settings
    app.settings = result.settings
    app.toast('success', t('更新通道已切换'))
  } catch (error) {
    app.fromError(error, t('保存设置失败'))
  }
}

async function checkUpdate() {
  checkingUpdate.value = true
  updateInfo.value = null
  updateJob.value = null
  try {
    const info = await updatesApi.check()
    updateInfo.value = info
    if (info.code === 'UPDATE_AVAILABLE') updateModalOpen.value = true
    else if (info.code === 'UP_TO_DATE') app.toast('success', t('已是最新版本'))
    else app.toast('warning', info.message || t('检查更新失败'))
  } catch (error) {
    app.fromError(error, t('检查更新失败'))
  } finally {
    checkingUpdate.value = false
  }
}

async function startUpdateDownload() {
  updateModalOpen.value = false
  downloadingUpdate.value = true
  try {
    const result = await updatesApi.download()
    updateJob.value = {
      id: result.jobId, status: 'downloading', statusLabel: t('下载中'), progress: 0,
      message: result.message, code: '', version: updateInfo.value?.latestVersion || '',
      assetName: updateInfo.value?.assetName || '', downloadedBytes: 0,
      totalBytes: updateInfo.value?.assetSize || 0, scriptPath: ''
    }
    pollUpdateJob(result.jobId)
  } catch (error) {
    app.fromError(error, t('下载更新失败'))
  } finally {
    downloadingUpdate.value = false
  }
}

function pollUpdateJob(id: string) {
  if (updatePoll) window.clearInterval(updatePoll)
  updatePoll = window.setInterval(async () => {
    try {
      const job = await updatesApi.job(id)
      updateJob.value = job
      if (job.status === 'ready' || job.status === 'failed' || job.status === 'applied') {
        window.clearInterval(updatePoll)
        updatePoll = undefined
        if (job.status === 'failed') app.toast('warning', job.message || t('更新失败'))
        else if (job.status === 'ready') app.toast('success', t('更新包已就绪'))
      }
    } catch (error) {
      if (updatePoll) window.clearInterval(updatePoll)
      updatePoll = undefined
      app.fromError(error, t('查询更新状态失败'))
    }
  }, 1000)
}

async function applyUpdate() {
  if (!updateJob.value) return
  applyingUpdate.value = true
  try {
    const result = await updatesApi.apply(updateJob.value.id)
    updateJob.value.status = 'applied'
    updateJob.value.statusLabel = t('已安排')
    app.toast('success', result.message || t('更新已安排'))
    const previous = currentVersion.value
    let attempts = 0
    if (versionPoll) window.clearInterval(versionPoll)
    versionPoll = window.setInterval(async () => {
      attempts += 1
      try {
        await app.refreshHealth()
        const next = app.health?.version || ''
        if (next && next !== previous) {
          window.clearInterval(versionPoll)
          versionPoll = undefined
          app.toast('success', t('当前版本已更新'), next)
        } else if (attempts >= 15) {
          window.clearInterval(versionPoll)
          versionPoll = undefined
        }
      } catch { /* backend may be restarting */ }
    }, 2000)
  } catch (error) {
    app.fromError(error, t('应用更新失败'))
  } finally {
    applyingUpdate.value = false
  }
}

onUnmounted(() => {
  if (updatePoll) window.clearInterval(updatePoll)
  if (versionPoll) window.clearInterval(versionPoll)
})
onMounted(async () => { await app.init(); await refreshVersion(); await load() })
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('系统设置') }}</h1>
        <p class="page-subtitle">{{ t('工具链和目录配置保存在本地 SQLite；修改后由同一核心逻辑供 Web UI 与 CLI 使用。') }}</p>
      </div>
      <div class="page-actions">
        <button class="btn" @click="checkUpdate" :disabled="checkingUpdate"><span v-if="checkingUpdate" class="spinner"></span><Icon v-else name="refresh" :size="13" /> {{ t('检查更新') }}</button>
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
            <div class="setting-group-copy">{{ t('量化类型从 llama-quantize --help 动态解析。') }}</div>
            <div class="notice warning" style="margin-bottom: 10px">
              <Icon name="info" :size="13" class="notice-icon" />
              <div>
                {{ t('随包分发的 llama.cpp 为官方 release 原样解压产物。') }}
                <router-link to="/runtime" style="color: var(--accent); text-decoration: underline">{{ t('打开“运行引擎”') }}</router-link> {{ t('可查看官方版本、检查更新并按当前平台下载官方资源。') }}
              </div>
            </div>
            <div class="field">
              <label class="field-label">{{ t('llama.cpp 目录') }}</label>
              <div class="input-with-button">
                <input v-model="form.llamaRoot" class="input mono" />
                <button class="btn btn-sm" @click="openDirPicker('llamaRoot')"><Icon name="folder" :size="12" /> {{ t('浏览') }}</button>
              </div>
              <div class="field-hint">{{ t('官方 zip 保存在 downloads/，原样解压到 releases/<资产名>/。') }}</div>
            </div>
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
            <div class="field">
              <label class="field-label">{{ t('默认模型目录') }}</label>
              <div class="input-with-button">
                <input v-model="form.defaultModelDir" class="input mono" />
                <button class="btn btn-sm" @click="openDirPicker('defaultModelDir')"><Icon name="folder" :size="12" /> {{ t('浏览') }}</button>
              </div>
            </div>
            <div class="field">
              <label class="field-label">{{ t('默认输出目录') }}</label>
              <div class="input-with-button">
                <input v-model="form.defaultOutputDir" class="input mono" />
                <button class="btn btn-sm" @click="openDirPicker('defaultOutputDir')"><Icon name="folder" :size="12" /> {{ t('浏览') }}</button>
              </div>
              <div class="field-hint">{{ t('转换前仍会检查该目录的可用磁盘空间与写权限。') }}</div>
            </div>
            <div class="field">
              <label class="field-label">{{ t('临时目录') }}</label>
              <div class="input-with-button">
                <input v-model="form.tempDir" class="input mono" />
                <button class="btn btn-sm" @click="openDirPicker('tempDir')"><Icon name="folder" :size="12" /> {{ t('浏览') }}</button>
              </div>
            </div>
            <div class="kv"><span>{{ t('数据库与日志目录') }}</span><span>{{ form.dataDir }}</span></div>
          </div>

          <div class="setting-group">
            <div class="setting-group-title">{{ t('调度') }}</div>
            <div class="field-row">
              <div class="field"><label class="field-label">{{ t('CPU 线程数') }}</label><input v-model.number="form.threads" type="number" min="1" max="512" class="input" /></div>
              <div class="field"><label class="field-label">{{ t('最大并发任务') }}</label><input v-model.number="form.maxConcurrent" type="number" min="1" max="8" class="input" /></div>
            </div>
            <label class="check"><input v-model="form.autoOpenBrowser" type="checkbox" /><span>{{ t('启动时自动打开浏览器') }}<small>{{ t('关闭后不再自动打开；命令行 --no-browser 只影响本次启动。') }}</small></span></label>
          </div>

          <div class="setting-group">
            <div class="setting-group-title">{{ t('Web 服务') }}</div>
            <div class="field-row">
              <div class="field"><label class="field-label">{{ t('监听地址') }}</label><input v-model="form.bindAddress" class="input mono" disabled /><div class="field-hint">{{ t('当前构建固定监听本机回环地址。') }}</div></div>
              <div class="field"><label class="field-label">{{ t('端口') }}</label><input v-model.number="form.port" type="number" min="1024" max="65535" class="input" /><div class="field-hint">{{ t('端口变更后需重启生效（1024-65535）。被占用时本次启动会临时换用空闲端口，不会改写此设置。') }}</div></div>
            </div>
            <div class="kv"><span>{{ t('当前实际端口') }}</span><span class="mono">{{ app.health?.port || form.port }}</span></div>
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
                <select v-model="form.language" class="select" @change="onLanguageChange(($event.target as HTMLSelectElement).value)">
                  <option value="zh-CN">简体中文</option>
                  <option value="en-US">English</option>
                </select>
                <div class="field-hint">{{ t('切换后立即生效并自动保存。') }}</div>
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

    <div class="card" style="margin-top: 14px">
      <div class="card-header">
        <div>
          <div class="card-title">{{ t('软件更新') }}</div>
          <div class="card-subtitle">{{ t('从 GitHub Releases 检查新版本，确认后下载并安装') }}</div>
        </div>
        <span class="badge">{{ t('当前版本') }} {{ currentVersion || '—' }}</span>
      </div>
      <div class="card-body">
        <div class="field" style="max-width: 420px; margin-bottom: 12px">
          <label class="field-label">{{ t('更新通道') }}</label>
          <select class="select" :value="form?.updateChannel || 'stable'" @change="onUpdateChannelChange(($event.target as HTMLSelectElement).value)">
            <option value="stable">{{ t('稳定版（优先正式 release）') }}</option>
            <option value="nightly">{{ t('每日构建（含预发布）') }}</option>
            <option value="auto">{{ t('自动（最新可安装构建）') }}</option>
          </select>
          <div class="field-hint">{{ t('稳定版只接收正式发布；每日构建优先预发布；自动取版本号最高的构建。') }}</div>
        </div>
        <div v-if="!updateInfo" class="setting-group-copy">{{ t('点击“检查更新”从 GitHub 获取最新版本信息。') }}</div>
        <div v-else-if="updateInfo.code === 'UP_TO_DATE'" class="notice">
          <Icon name="check" :size="14" class="notice-icon" />
          <div>{{ t('已是最新版本') }} · {{ updateInfo.latestVersion }}</div>
        </div>
        <div v-else-if="updateInfo.code !== 'UPDATE_AVAILABLE'" class="notice warning">
          <Icon name="warning" :size="14" class="notice-icon" />
          <div>{{ updateInfo.message }}<span v-if="updateInfo.suggestion"> · {{ updateInfo.suggestion }}</span></div>
        </div>
        <div v-else>
          <div class="kv"><span>{{ t('最新版本') }}</span><span>{{ updateInfo.latestVersion }} · {{ updateInfo.releaseName }}</span></div>
          <div class="kv"><span>{{ t('发布时间') }}</span><span>{{ formatTime(updateInfo.publishedAt) }}</span></div>
          <div class="kv"><span>{{ t('当前平台安装包') }}</span><span>{{ updateInfo.assetName }} ({{ humanBytes(updateInfo.assetSize) }})</span></div>
          <pre class="update-notes" style="white-space: pre-wrap; max-height: 220px; overflow: auto; margin: 10px 0; padding: 10px; border: 1px solid var(--border); border-radius: 8px; font-size: 12px;">{{ updateInfo.releaseNotes }}</pre>
          <div class="page-actions">
            <a class="btn" :href="updateInfo.htmlUrl" target="_blank" rel="noopener"><Icon name="download" :size="13" /> {{ t('打开发布页') }}</a>
            <button class="btn btn-primary" :disabled="downloadingUpdate || !!updateJob" @click="startUpdateDownload">
              <span v-if="downloadingUpdate" class="spinner"></span>
              <Icon v-else name="download" :size="13" /> {{ t('下载并安装') }}
            </button>
          </div>
        </div>

        <div v-if="updateJob" class="notice" style="margin-top: 10px; display: block">
          <div class="kv"><span>{{ t('更新状态') }}</span><span>{{ updateJob.statusLabel }} · {{ updateJob.progress }}%</span></div>
          <ProgressBar :value="updateJob.progress" :label="updateJob.message" :meta="updateJob.assetName" />
          <div v-if="updateJob.status === 'ready'" class="page-actions" style="margin-top: 10px">
            <button class="btn btn-primary" :disabled="applyingUpdate" @click="applyUpdate">
              <span v-if="applyingUpdate" class="spinner"></span>
              <Icon v-else name="check" :size="13" /> {{ t('确认安装') }}
            </button>
          </div>
          <div v-if="updateJob.scriptPath" class="field-hint">{{ t('更新脚本') }}：{{ updateJob.scriptPath }}</div>
        </div>
      </div>
    </div>

    <DirectoryPicker v-model="dirPickerOpen" :initial-path="dirPickerInitial" @select="onDirSelected" />

    <AppModal v-model="updateModalOpen" :title="t('发现新版本')" :subtitle="t('请确认是否下载并安装该版本')" :wide="true">
      <div v-if="updateInfo">
        <div class="kv"><span>{{ t('当前版本') }}</span><span>{{ currentVersion }}</span></div>
        <div class="kv"><span>{{ t('最新版本') }}</span><span>{{ updateInfo.latestVersion }}</span></div>
        <div class="kv"><span>{{ t('安装包') }}</span><span>{{ updateInfo.assetName }} ({{ humanBytes(updateInfo.assetSize) }})</span></div>
        <div class="setting-group-title" style="margin-top: 12px">{{ t('更新说明') }}</div>
        <pre class="update-notes" style="white-space: pre-wrap; max-height: 260px; overflow: auto;">{{ updateInfo.releaseNotes }}</pre>
      </div>
      <template #footer>
        <button class="btn" @click="updateModalOpen = false">{{ t('取消') }}</button>
        <button class="btn btn-primary" @click="startUpdateDownload">{{ t('确认下载并安装') }}</button>
      </template>
    </AppModal>

  </div>
</template>
