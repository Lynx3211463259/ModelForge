<script setup lang="ts">
import { t } from '../i18n'
import { nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import * as echarts from 'echarts'
import { chartPalette, themeMode } from '../theme'

const props = withDefaults(defineProps<{ option: Record<string, any>; height?: number }>(), { height: 260 })
const el = ref<HTMLDivElement | null>(null)
let chart: echarts.ECharts | null = null
let observer: ResizeObserver | null = null

function render() {
  if (!el.value) return
  if (!chart) chart = echarts.init(el.value, undefined, { renderer: 'canvas' })
  const palette = themeMode.value ? chartPalette() : chartPalette()
  chart.setOption({
    animationDuration: 350,
    textStyle: { color: palette.text, fontFamily: 'Inter, Segoe UI, sans-serif', fontSize: 10 },
    ...props.option
  }, true)
}

onMounted(async () => {
  await nextTick()
  render()
  if (el.value) {
    observer = new ResizeObserver(() => chart?.resize())
    observer.observe(el.value)
  }
})

watch(() => props.option, () => render(), { deep: true })
onBeforeUnmount(() => {
  observer?.disconnect()
  chart?.dispose()
  chart = null
})
</script>

<template>
  <div ref="el" class="chart" :style="{ height: height + 'px' }"></div>
</template>
