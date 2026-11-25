<script setup lang="ts">
import { t } from '../i18n'
import { nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import Icon from './Icon.vue'
import { createModalId, isTopModal, popModal, pushModal } from './modalStack'

const props = withDefaults(defineProps<{
  modelValue: boolean
  title: string
  subtitle?: string
  wide?: boolean
  closeable?: boolean
}>(), { subtitle: '', wide: false, closeable: true })
const emit = defineEmits<{ 'update:modelValue': [value: boolean] }>()

// Ids must come from the shared module: a per-instance counter would give every
// dialog the same id and collapse the stack.
const id = createModalId()
const zIndex = ref(100)
const dialog = ref<HTMLElement | null>(null)
let returnFocus: HTMLElement | null = null

function enterStack() {
  zIndex.value = pushModal(id)
  returnFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
  nextTick(() => dialog.value?.focus())
}

function leaveStack() {
  popModal(id)
  if (returnFocus && returnFocus.isConnected) returnFocus.focus()
  returnFocus = null
}

function close() {
  if (props.closeable) emit('update:modelValue', false)
}

function onKey(event: KeyboardEvent) {
  // Only the topmost dialog reacts. Marking the event prevents the listener of
  // the dialog underneath from handling the same Escape after the top one pops.
  if (event.defaultPrevented) return
  if (event.key === 'Escape' && props.modelValue && isTopModal(id)) {
    event.preventDefault()
    close()
  }
}

watch(
  () => props.modelValue,
  (open) => {
    if (open) enterStack()
    else leaveStack()
  },
  { immediate: true }
)

onMounted(() => window.addEventListener('keydown', onKey))
onUnmounted(() => {
  window.removeEventListener('keydown', onKey)
  leaveStack()
})
</script>

<template>
  <Teleport to="body">
    <div v-if="modelValue" class="modal-backdrop" :style="{ zIndex }" @click.self="close">
      <section ref="dialog" class="modal" :class="{ wide }" role="dialog" aria-modal="true" tabindex="-1">
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
