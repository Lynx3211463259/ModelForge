<script setup lang="ts">
withDefaults(defineProps<{
  value: number
  mode?: string
  label?: string
  meta?: string
  danger?: boolean
}>(), { mode: 'measured', label: '', meta: '', danger: false })
</script>

<template>
  <div>
    <div class="progress">
      <div
        class="progress-bar"
        :class="{ estimated: mode === 'estimated' || mode === 'stage', indeterminate: value < 0, danger }"
        :style="{ width: value >= 0 ? Math.min(100, Math.max(0, value)) + '%' : '28%' }"
      ></div>
    </div>
    <div v-if="label || meta" class="progress-meta">
      <span>{{ label }}</span>
      <span>{{ meta }}</span>
    </div>
  </div>
</template>
