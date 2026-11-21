<script setup lang="ts">
import Icon from './Icon.vue'
withDefaults(defineProps<{ icon?: string; title: string; copy?: string }>(), { icon: 'models', copy: '' })
</script>

<template>
  <div class="empty-state">
    <div class="empty-icon"><Icon :name="icon" :size="22" /></div>
    <div class="empty-title">{{ title }}</div>
    <div v-if="copy" class="empty-copy">{{ copy }}</div>
    <slot />
  </div>
</template>
