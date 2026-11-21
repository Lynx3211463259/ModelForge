<script setup lang="ts">
import { t } from '../i18n'
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { api, humanBytes, humanNumber } from '../api/client'
import type { EstimateOption, Model } from '../types'
import { useAppStore } from '../stores/app'
import Icon from '../components/Icon.vue'
import ChartPanel from '../components/ChartPanel.vue'
import EmptyState from '../components/EmptyState.vue'
import StatusBadge from '../components/StatusBadge.vue'

const route = useRoute()
const router = useRouter()
const app = useAppStore()
const models = ref<Model[]>([])
const selectedId = ref('')
const model = ref<Model | null>(null)
const estimates = ref<EstimateOption[]>([])
const loading = ref(false)
const analyzing = ref(false)
const recommendMode = ref<'quality' | 'balanced' | 'size'>('balanced')
const recommendation = ref<Record<string, any> | null>(null)

const analysis = computed(() => model.value?.analysis || {})
const checks = computed(() => analysis.value.checks || [])
const configSummary = computed(() => analysis.value.configSummary || {})

const chartOption = computed(() => {
  const available = estimates.value.filter((item) => item.available).slice(0, 9)
  return {
    grid: { left: 48, right: 18, top: 26, bottom: 34 },
    tooltip: {
      trigger: 'axis',
      backgroundColor: '#0e141d',
      borderColor: '#28364a',
      textStyle: { color: '#dfe7f1', fontSize: 10 },
      valueFormatter: (value: number) => humanBytes(value)
    },
    xAxis: {
      type: 'category',
      data: available.map((item) => item.type),
      axisLine: { lineStyle: { color: '#1e2937' } },
      axisTick: { show: false },
      axisLabel: { color: '#748196', fontSize: 9 }
    },
    yAxis: {
      type: 'value',
      splitLine: { lineStyle: { color: '#17212d' } },
      axisLabel: { color: '#526074', fontSize: 9, formatter: (value: number) => humanBytes(value) }
    },
    series: [{
      type: 'bar',
      data: available.map((item) => ({
        value: item.estimatedSize,
        itemStyle: { color: item.type === 'Q4_K_M' ? '#34d6c4' : '#326f9f', borderRadius: [4, 4, 0, 0] }
      })),
      barMaxWidth: 28
    }]
  }
})

async function loadModels() {
  try {
    models.value = await api.get<Model[]>('/api/v1/models?limit=500')
    if (!models.value.length) return
    const queryId = typeof route.query.id === 'string' ? route.query.id : ''
    selectedId.value = models.value.some((item) => item.id === queryId) ? queryId : models.value[0].id
    await loadModel(selectedId.value)
  } catch (error) {
    app.fromError(error, t('加载模型失败'))
  }
}

async function loadModel(id: string) {
  if (!id) return
  loading.value = true
  recommendation.value = null
  try {
    model.value = await api.get<Model>(`/api/v1/models/${id}`)
    const estimateResult = await api.get<{ options: EstimateOption[] }>(`/api/v1/models/${id}/estimates`)
    estimates.value = estimateResult.options || []
    router.replace({ query: { id } })
  } catch (error) {
    app.fromError(error, t('加载分析失败'))
  } finally {
    loading.value = false
  }
}

async function reanalyze() {
  if (!model.value) return
  analyzing.value = true
  try {
    model.value = await api.post<Model>(`/api/v1/models/${model.value.id}/analyze`)
    const result = await api.get<{ options: EstimateOption[] }>(`/api/v1/models/${model.value.id}/estimates`)
    estimates.value = result.options || []
    app.toast('success', t('模型分析已刷新'))
  } catch (error) {
    app.fromError(error, t('重新分析失败'))
  } finally {
    analyzing.value = false
  }
}

async function recommend() {
  if (!model.value) return
  try {
    recommendation.value = await api.post(`/api/v1/models/${model.value.id}/recommend`, { mode: recommendMode.value })
  } catch (error) {
    app.fromError(error, t('量化推荐失败'))
  }
}

watch(selectedId, (id, old) => { if (id && id !== old) loadModel(id) })
onMounted(loadModels)
</script>

<template>
  <div>
    <div class="page-intro">
      <div>
        <h1 class="page-title">{{ t('模型分析器') }}</h1>
        <p class="page-subtitle">{{ t('从配置和张量头部推导结构、完整性、转换兼容性与体积空间。') }}</p>
      </div>
      <div class="page-actions">
        <select v-if="models.length" v-model="selectedId" class="select" style="width: 260px">
          <option v-for="item in models" :key="item.id" :value="item.id">{{ item.name }}</option>
        </select>
        <button class="btn" :disabled="!model || analyzing" @click="reanalyze">
          <Icon name="refresh" :size="13" /> {{ t('{0}', [analyzing ? t('重新扫描…') : t('重新分析')]) }}</button>
        <router-link v-if="model" :to="{ path: '/convert', query: { model: model.id } }" class="btn btn-primary">
          {{ t('转换此模型') }} <Icon name="arrow" :size="13" />
        </router-link>
      </div>
    </div>

    <div v-if="!models.length" class="card">
      <EmptyState icon="analyzer" :title="t('模型库为空')" copy="先导入一个完整模型目录，再查看结构、参数与完整性报告。">
        <router-link to="/models" class="btn btn-primary">{{ t('前往导入') }}</router-link>
      </EmptyState>
    </div>
    <div v-else-if="loading" class="loading-panel"><span class="spinner"></span> {{ t('正在重新检查模型结构与配置…') }}</div>

    <template v-else-if="model">
      <div class="grid grid-2" style="grid-template-columns: minmax(0, 0.85fr) minmax(0, 1.15fr); margin-bottom: 14px">
        <div class="card">
          <div class="card-header">
            <div class="model-avatar">{{ (model.architectureFamily || 'AI').slice(0, 2).toUpperCase() }}</div>
            <div style="min-width: 0">
              <div class="card-title truncate">{{ model.name }}</div>
              <div class="card-subtitle truncate mono">{{ model.path }}</div>
            </div>
            <div class="card-header-actions"><StatusBadge :status="model.status" /></div>
          </div>
          <div class="card-body">
            <div class="kv-grid">
              <div class="kv"><span>{{ t('架构') }}</span><span>{{ model.architecture }}</span></div>
              <div class="kv"><span>{{ t('家族') }}</span><span>{{ model.architectureFamily }}</span></div>
              <div class="kv"><span>{{ t('参数规模') }}</span><span>{{ humanNumber(model.parameterCount) }}</span></div>
              <div class="kv"><span>{{ t('权重精度') }}</span><span>{{ model.dtype || '—' }}</span></div>
              <div class="kv"><span>{{ t('层数') }}</span><span>{{ analysis.layers || '—' }}</span></div>
              <div class="kv"><span>{{ t('注意力头') }}</span><span>{{ analysis.heads || '—' }} / KV {{ analysis.kvHeads || '—' }}</span></div>
              <div class="kv"><span>Hidden Size</span><span>{{ analysis.hiddenSize || '—' }}</span></div>
              <div class="kv"><span>Intermediate</span><span>{{ analysis.intermediateSize || '—' }}</span></div>
              <div class="kv"><span>{{ t('词表') }}</span><span>{{ analysis.vocabSize || '—' }}</span></div>
              <div class="kv"><span>{{ t('上下文') }}</span><span>{{ analysis.contextLength || '—' }}</span></div>
              <div class="kv"><span>{{ t('分片 / 张量') }}</span><span>{{ model.shardCount }} / {{ model.tensorCount }}</span></div>
              <div class="kv"><span>{{ t('模型体积') }}</span><span>{{ humanBytes(model.totalSize) }}</span></div>
            </div>
            <div class="notice" :class="model.supported ? 'success' : 'warning'" style="margin-top: 14px">
              <Icon :name="model.supported ? 'check' : 'warning'" :size="14" class="notice-icon" />
              <div>
                <strong>{{ t('{0}', [model.supported ? t('当前转换器注册了该架构') : t('尚未确认架构兼容性')]) }}</strong><br /> {{ t('{0}', [model.supported ? t('可继续创建转换任务；转换前仍会检查配置、分片、Tokenizer 与磁盘空间。') : t('请在设置中验证 llama.cpp 转换脚本。ModelForge 不会强行执行不兼容转换。')]) }}</div>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><div><div class="card-title">{{ t('结构剖面') }}</div><div class="card-subtitle">{{ t('基于 config.json 与实际张量头部的示意结构') }}</div></div></div>
          <div class="card-body" style="padding: 8px 16px 16px">
            <div class="layer-stack">
              <div class="layer"><span>Token Embedding</span><span class="layer-dim">{{ analysis.hiddenSize || '?' }} × {{ analysis.vocabSize || '?' }}</span></div>
              <div class="layer-repeat">
                <div class="layer-repeat-head">TRANSFORMER BLOCK × {{ analysis.layers || '?' }}</div>
                <div class="layer-sub"><span>Self Attention</span><span>{{ analysis.heads || '?' }} heads · {{ analysis.kvHeads || '?' }} KV</span></div>
                <div class="layer-sub"><span>Feed Forward</span><span>{{ analysis.intermediateSize || '?' }} intermediate</span></div>
              </div>
              <div class="layer"><span>Final Norm</span><span class="layer-dim">{{ configSummary.hiddenAct || 'activation' }}</span></div>
              <div class="layer primary"><span>LM Head</span><span class="layer-dim">{{ analysis.vocabSize || '?' }} logits</span></div>
            </div>
            <div class="faint tiny" style="text-align: center; margin-top: 9px">
              {{ configSummary.modelType || model.architecture }} · rope θ {{ configSummary.ropeTheta || '—' }} · {{ configSummary.tieWordEmbeddings ? 'tied embeddings' : 'untied embeddings' }}
            </div>
          </div>
        </div>
      </div>

      <div class="split-panel" style="margin-bottom: 14px">
        <div class="card">
          <div class="card-header"><div><div class="card-title">{{ t('完整性检查') }}</div><div class="card-subtitle">{{ t('转换前硬性条件') }}</div></div></div>
          <div class="card-body">
            <div v-if="!checks.length" class="notice">{{ t('尚无检查明细，请点击“重新分析”。') }}</div>
            <div v-else class="checklist">
              <div v-for="check in checks" :key="check.id" class="check-item">
                <span class="check-mark" :class="check.status">
                  <Icon :name="check.status === 'pass' ? 'check' : 'close'" :size="9" :stroke="2" />
                </span>
                <div><div class="check-title">{{ check.label }}</div><div class="check-detail">{{ check.detail }}</div></div>
              </div>
            </div>
            <div v-if="analysis.missing?.length" class="notice danger" style="margin-top: 10px">
              <Icon name="warning" :size="14" class="notice-icon" />
              <div>{{ t('缺失分片：') }}<span class="mono">{{ analysis.missing.join(', ') }}</span></div>
            </div>
            <div v-for="warning in analysis.warnings || []" :key="warning" class="notice warning" style="margin-top: 8px">
              <Icon name="info" :size="14" class="notice-icon" /><div>{{ warning }}</div>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header">
            <div><div class="card-title">{{ t('量化体积空间') }}</div><div class="card-subtitle">{{ t('按参数规模与每权重比特数估算') }}</div></div>
            <div class="card-header-actions"><span class="tag">ESTIMATE</span></div>
          </div>
          <div class="card-body">
            <ChartPanel :option="chartOption" :height="250" />
            <div class="faint tiny" style="margin-top: 4px">{{ t('{0}', [estimates.length ? t('实际体积随模型结构、嵌入层和混合量化策略变化。') : t('参数规模未知，无法生成体积估算。')]) }}</div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-header">
          <div><div class="card-title">{{ t('智能量化建议') }}</div><div class="card-subtitle">{{ t('基于模型信息与当前可用内存的启发式推荐，不代替真实基准测试') }}</div></div>
          <div class="card-header-actions">
            <div style="display: flex; gap: 4px">
              <button class="chip" :class="{ active: recommendMode === 'quality' }" @click="recommendMode = 'quality'; recommend()">{{ t('质量') }}</button>
              <button class="chip" :class="{ active: recommendMode === 'balanced' }" @click="recommendMode = 'balanced'; recommend()">{{ t('平衡') }}</button>
              <button class="chip" :class="{ active: recommendMode === 'size' }" @click="recommendMode = 'size'; recommend()">{{ t('体积') }}</button>
            </div>
          </div>
        </div>
        <div class="card-body">
          <div v-if="!recommendation" style="display: flex; align-items: center; gap: 10px; color: var(--muted); font-size: 11.5px">
            {{ t('选择意图后生成推荐。') }}
            <button class="btn btn-sm" @click="recommend">{{ t('生成建议') }}</button>
          </div>
          <template v-else>
            <div class="insight">{{ recommendation.reason }}</div>
            <div v-if="recommendation.recommended" class="grid grid-3" style="margin-top: 14px">
              <div class="well">
                <div class="faint tiny">{{ t('推荐类型') }}</div>
                <div style="font-family: var(--font-mono); font-size: 21px; color: var(--accent); margin: 4px 0">{{ recommendation.recommended.type }}</div>
                <div class="small muted">{{ recommendation.recommended.description }}</div>
              </div>
              <div class="well">
                <div class="faint tiny">{{ t('预计输出体积') }}</div>
                <div style="font-size: 21px; margin: 4px 0">{{ humanBytes(recommendation.recommended.estimatedSize) }}</div>
                <div class="small muted">{{ recommendation.recommended.bitsPerWeight }} bits/weight</div>
              </div>
              <div class="well">
                <div class="faint tiny">{{ t('估算运行内存') }}</div>
                <div style="font-size: 21px; margin: 4px 0">{{ humanBytes(recommendation.recommended.estimatedRuntimeMemory) }}</div>
                <div class="small muted">{{ t('可用 {0}', [humanBytes(recommendation.availableMemory)]) }}</div>
              </div>
            </div>
            <div v-if="recommendation.resourceWarning" class="notice warning" style="margin-top: 10px">
              <Icon name="warning" :size="14" class="notice-icon" /> {{ recommendation.resourceWarning }}
            </div>
            <div class="faint tiny" style="margin-top: 10px">{{ t(recommendation.method) }}</div>
          </template>
        </div>
      </div>
    </template>
  </div>
</template>
