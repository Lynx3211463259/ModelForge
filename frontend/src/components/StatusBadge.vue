<script setup lang="ts">
import { statusLabel, statusTone } from '../api/client'
defineProps<{ status: string; label?: string }>()
</script>

<template>
  <span class="badge" :class="statusTone(status)">{{ t(label || statusLabel(status)) }}</span>
</template>
