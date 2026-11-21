<script setup lang="ts">
import { t } from '../i18n'
import { onMounted, onUnmounted, watch } from 'vue'
import Icon from './Icon.vue'

const props = withDefaults(defineProps<{
  modelValue: boolean
  title: string
  subtitle?: string
  wide?: boolean
  closeable?: boolean
}>(), { subtitle: '', wide: false, closeable: true })
const emit = defineEmits<{ 'update:modelValue': [value: boolean] }>()

function close() {
  if (props.closeable) emit('update:modelValue', false)
}

function onKey(event: KeyboardEvent) {
  if (event.key === 'Escape' && props.modelValue) close()
}

onMounted(() => window.addEventListener('keydown', onKey))
onUnmounted(() => window.removeEventListener('keydown', onKey))
watch(() => props.modelValue, (open) => {
  document.body.style.overflow = open ? 'hidden' : ''
})
</script>

<template>
  <Teleport to="body">
    <div v-if="modelValue" class="modal-backdrop" @click.self="close">
      <section class="modal" :class="{ wide }" role="dialog" aria-modal="true">
        <header class="modal-head">
          <div>
            <div class="modal-title">{{ title }}</div>
            <div v-if="subtitle" class="modal-sub">{{ subtitle }}</div>
          </div>
          <button v-if="closeable" class="icon-btn modal-close" @click="close">
            <Icon name="close" :size="14" />
          </button>
        </header>
        <div class="modal-body">
          <slot />
        </div>
        <footer v-if="$slots.footer" class="modal-foot">
          <slot name="footer" />
        </footer>
      </section>
    </div>
  </Teleport>
</template>
