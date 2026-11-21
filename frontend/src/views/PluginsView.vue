<script setup lang="ts">
import { t } from '../i18n'
import { onMounted, ref } from 'vue'
import { api } from '../api/client'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'

const app = useAppStore()
const engines = ref<any[]>([])
const plugins = ref<any[]>([])
const note = ref('')
const loading = ref(true)

async function load() {
  loading.value = true
  try {
    const data = await api.get<{ engines: any[]; plugins: any[]; note: string }>('/api/v1/plugins/engines')
    engines.value = data.engines
    plugins.value = data.plugins
    note.value = data.note
  } catch (error) {
    app.fromError(error, t('读取引擎状态失败'))
  } finally {
    loading.value = false
  }
}

onMounted(load)
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('插件与引擎') }}</h1>
        <p class="page-subtitle">{{ t('查看转换引擎能力、版本与扩展接口。未知二进制不会在当前进程中自动加载。') }}</p>
      </div>
      <div class="page-actions">
        <button class="btn" @click="load"><Icon name="refresh" :size="13" /> {{ t('重新检测') }}</button>
        <button class="btn" disabled><Icon name="plus" :size="13" /> {{ t('安装插件') }}</button>
      </div>
    </div>

    <div v-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在检测引擎能力…') }}</div>
    <template v-else>
      <div class="grid grid-2" style="margin-bottom: 14px">
        <article v-for="engine in engines" :key="engine.id" class="card">
          <div class="card-header">
            <div class="model-avatar">{{ engine.id === 'llama.cpp' ? 'GG' : 'Q' }}</div>
            <div style="flex: 1">
              <div class="card-title">{{ engine.name }}</div>
              <div class="card-subtitle mono">{{ t('{0}', [engine.version || t('未检测到版本')]) }}</div>
            </div>
            <span class="badge" :class="engine.ready ? 'success' : 'danger'">{{ engine.ready ? 'READY' : 'UNAVAILABLE' }}</span>
          </div>
          <div class="card-body">
            <div class="kv"><span>{{ t('类型') }}</span><span>{{ engine.kind }}</span></div>
            <div class="kv"><span>{{ t('安装状态') }}</span><span>{{ t('{0}', [engine.installed ? t('已发现') : t('未找到')]) }}</span></div>
            <div class="kv"><span>{{ t('可执行能力') }}</span><span>{{ t('{0}', [engine.ready ? t('可用') : t('被依赖或路径阻断')]) }}</span></div>
            <div v-if="engine.capabilities?.architectures" class="kv"><span>{{ t('架构注册') }}</span><span>{{ engine.capabilities.architectures }} entries</span></div>
            <div v-if="engine.capabilities?.outTypes" class="kv"><span>{{ t('输出精度') }}</span><span>{{ (engine.capabilities.outTypes || []).join(', ') }}</span></div>
            <div v-if="engine.capabilities?.quantTypes" style="margin-top: 9px">
              <div class="field-label">{{ t('动态量化类型') }} <span class="mono">{{ engine.capabilities.quantTypes.length }}</span></div>
              <div style="display: flex; gap: 4px; flex-wrap: wrap; max-height: 96px; overflow: auto">
                <span v-for="type in engine.capabilities.quantTypes.slice(0, 32)" :key="type" class="tag">{{ type }}</span>
              </div>
            </div>
          </div>
        </article>
      </div>

      <div class="card">
        <div class="card-header"><div><div class="card-title">{{ t('外部插件清单') }}</div><div class="card-subtitle">{{ t('读取 plugins/*.json，但不在进程内执行未知代码') }}</div></div><span class="badge muted">{{ plugins.length }} FOUND</span></div>
        <div v-if="!plugins.length" class="empty-state" style="padding: 34px">
          <div class="empty-icon"><Icon name="plugins" :size="21" /></div>
          <div class="empty-title">{{ t('暂无外部插件') }}</div>
          <div class="empty-copy">{{ t('插件接口仍在演进。为了安全，ModelForge 只识别转换引擎适配器，不会从任意路径加载未签名 DLL。') }}</div>
        </div>
        <div v-else class="card-body">
          <div v-for="plugin in plugins" :key="plugin.id" class="plugin-row">
            <Icon name="plugins" :size="15" />
            <div style="flex: 1"><div class="strong">{{ plugin.name }}</div><div class="faint tiny">{{ plugin.description }}</div></div>
            <span class="tag">{{ plugin.version }}</span>
            <span class="badge muted">MANIFEST ONLY</span>
          </div>
        </div>
      </div>

      <div class="notice info" style="margin-top: 14px">
        <Icon name="info" :size="14" class="notice-icon" />
        <div>{{ note }}</div>
      </div>

      <div class="card" style="margin-top: 14px">
        <div class="card-header"><div><div class="card-title">{{ t('引擎适配器接口约定') }}</div><div class="card-subtitle">{{ t('为后续第三方转换工具预留的边界') }}</div></div></div>
        <div class="card-body">
          <div class="grid grid-3">
            <div class="well"><div class="option-name">detect()</div><div class="option-desc">{{ t('返回版本、可执行路径、支持架构与量化类型。') }}</div></div>
            <div class="well"><div class="option-name">validate()</div><div class="option-desc">{{ t('在启动前验证模型完整性与参数边界。') }}</div></div>
            <div class="well"><div class="option-name">buildPlan()</div><div class="option-desc">{{ t('生成阶段化命令与产物约定，不经过 Shell。') }}</div></div>
          </div>
          <div class="faint tiny" style="margin-top: 10px">{{ t('当前内置适配器已经采用该流程；外部插件的动态加载、签名验证和权限隔离尚未实现。') }}</div>
        </div>
      </div>
    </template>
  </div>
</template>

<style scoped>
.plugin-row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 11px 0;
  border-bottom: 1px solid var(--line-soft);
}
.plugin-row:last-child { border-bottom: none; }
</style>
