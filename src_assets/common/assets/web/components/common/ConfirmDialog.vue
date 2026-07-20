<script setup>
import { nextTick, onBeforeUnmount, ref, watch } from 'vue'
import { getFocusableElements } from '../../utils/focus.js'

const props = defineProps({
  show: { type: Boolean, default: false },
  dialogId: { type: String, required: true },
  title: { type: String, required: true },
  titleIcon: { type: String, default: '' },
  tone: { type: String, default: 'accent' },
  dismissible: { type: Boolean, default: true },
  closeLabel: { type: String, default: 'Close' },
  maxWidth: { type: String, default: '500px' },
})

const emit = defineEmits(['close'])
const dialog = ref(null)
let previousFocus = null
let previousBodyOverflow = null

const setBodyScrollLock = (locked) => {
  if (locked) {
    if (previousBodyOverflow === null) previousBodyOverflow = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return
  }

  if (previousBodyOverflow !== null) {
    document.body.style.overflow = previousBodyOverflow
    previousBodyOverflow = null
  }
}

const close = () => {
  if (props.dismissible) emit('close')
}

const handleKeydown = (event) => {
  if (event.key === 'Escape') {
    event.preventDefault()
    close()
    return
  }
  if (event.key !== 'Tab') return

  const focusable = getFocusableElements(dialog.value)

  if (!focusable.length) {
    event.preventDefault()
    dialog.value?.focus()
    return
  }

  const first = focusable[0]
  const last = focusable[focusable.length - 1]
  const activeIndex = focusable.indexOf(document.activeElement)
  if (event.shiftKey && activeIndex <= 0) {
    event.preventDefault()
    last.focus()
  } else if (!event.shiftKey && (activeIndex === -1 || document.activeElement === last)) {
    event.preventDefault()
    first.focus()
  }
}

watch(
  () => props.show,
  async (show) => {
    if (show) {
      setBodyScrollLock(true)
      previousFocus = document.activeElement
      await nextTick()
      dialog.value?.focus()
    } else {
      setBodyScrollLock(false)
      if (previousFocus instanceof HTMLElement) {
        previousFocus.focus()
        previousFocus = null
      }
    }
  },
  { immediate: true },
)

onBeforeUnmount(() => {
  setBodyScrollLock(false)
  if (previousFocus instanceof HTMLElement) previousFocus.focus()
})
</script>

<template>
  <Teleport to="body">
    <Transition name="confirm-dialog">
      <div v-if="show" class="confirm-dialog-overlay" @click.self="close">
        <section
          ref="dialog"
          class="confirm-dialog-panel"
          :class="`confirm-dialog-panel--${tone}`"
          :style="{ '--confirm-dialog-max-width': maxWidth }"
          role="dialog"
          aria-modal="true"
          :aria-labelledby="`${dialogId}-title`"
          tabindex="-1"
          @keydown="handleKeydown"
        >
          <header class="confirm-dialog-header">
            <h5 :id="`${dialogId}-title`">
              <i v-if="titleIcon" :class="titleIcon" aria-hidden="true"></i>
              {{ title }}
            </h5>
            <button
              v-if="dismissible"
              type="button"
              class="confirm-dialog-close"
              :aria-label="closeLabel"
              @click="close"
            >
              <i class="fas fa-times" aria-hidden="true"></i>
            </button>
          </header>
          <div class="confirm-dialog-body">
            <slot />
          </div>
          <footer v-if="$slots.actions" class="confirm-dialog-footer">
            <slot name="actions" />
          </footer>
        </section>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.confirm-dialog-overlay {
  position: fixed;
  inset: 0;
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 1.5rem);
  overflow: hidden;
  background: var(--ui-overlay);
  backdrop-filter: blur(8px);
}

.confirm-dialog-panel {
  width: min(var(--confirm-dialog-max-width), 100%);
  max-height: calc(100vh - 2.5rem);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-lg);
  outline: none;
  background: var(--ui-surface-strong);
  box-shadow: var(--ui-shadow-md);
  color: var(--ui-text-primary);
  backdrop-filter: blur(20px);
}

.confirm-dialog-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 1.25rem 1.5rem;
  border-bottom: 1px solid var(--ui-border);
}

.confirm-dialog-header h5 {
  display: flex;
  align-items: center;
  gap: 0.55rem;
  margin: 0;
  color: var(--ui-text-primary);
  font-size: 1.1rem;
  font-weight: 600;
}

.confirm-dialog-header h5 i {
  color: var(--ui-accent);
}

.confirm-dialog-panel--danger .confirm-dialog-header h5 i {
  color: var(--ui-danger);
}

.confirm-dialog-panel--warning .confirm-dialog-header h5 i {
  color: var(--ui-warning);
}

.confirm-dialog-close {
  width: 2rem;
  height: 2rem;
  display: inline-flex;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  padding: 0;
  border: 0;
  border-radius: 50%;
  background: transparent;
  color: var(--ui-text-secondary);
  cursor: pointer;
}

.confirm-dialog-close:hover,
.confirm-dialog-close:focus-visible {
  background: var(--ui-surface-hover);
  color: var(--ui-text-primary);
  outline: none;
  box-shadow: 0 0 0 3px var(--ui-accent-soft);
}

.confirm-dialog-body {
  min-height: 0;
  flex: 1;
  overflow-y: auto;
  padding: 1.5rem;
  color: var(--ui-text-secondary);
  line-height: 1.6;
}

.confirm-dialog-body :deep(p:last-child) {
  margin-bottom: 0;
}

.confirm-dialog-footer {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 0.75rem;
  padding: 1rem 1.5rem;
  border-top: 1px solid var(--ui-border);
  background: color-mix(in srgb, var(--ui-surface) 70%, transparent);
}

.confirm-dialog-enter-active,
.confirm-dialog-leave-active {
  transition: opacity 0.2s ease;
}

.confirm-dialog-enter-active .confirm-dialog-panel,
.confirm-dialog-leave-active .confirm-dialog-panel {
  transition: transform 0.2s ease, opacity 0.2s ease;
}

.confirm-dialog-enter-from,
.confirm-dialog-leave-to {
  opacity: 0;
}

.confirm-dialog-enter-from .confirm-dialog-panel,
.confirm-dialog-leave-to .confirm-dialog-panel {
  opacity: 0;
  transform: translateY(16px) scale(0.98);
}

@media (max-width: 575.98px) {
  .confirm-dialog-overlay {
    align-items: flex-end;
    padding: 0.75rem;
  }

  .confirm-dialog-panel {
    max-height: calc(100vh - 1.5rem);
    border-radius: var(--ui-radius-md);
  }

  .confirm-dialog-header,
  .confirm-dialog-body,
  .confirm-dialog-footer {
    padding: 1rem;
  }

  .confirm-dialog-footer {
    flex-direction: column-reverse;
  }

  .confirm-dialog-footer :deep(.btn) {
    width: 100%;
  }
}
</style>
