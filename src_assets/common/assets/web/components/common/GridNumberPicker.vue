<script setup>
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue'

/**
 * NumberGridPicker-NumberGrid Selector
 * A calendar table-style drop-down selector for selecting a value within a fixed integer range.
 * Support v-model bi-directional binding, automatically adapt Bootstrap light/dark theme.
 * Usage:
 *   <GridNumberPicker
 *     id="pair_max_attempts"
 *     v-model="config.pair_max_attempts"
 *     :min="0" :max="30" :columns="6"
 *     :placeholder="$t('config.pair_max_attempts_placeholder')"
 *   />
 */
const props = defineProps({
  modelValue: { type: [Number, String], default: 0 },
  min: { type: Number, default: 0 },
  max: { type: Number, default: 30 },
  columns: { type: Number, default: 6 },
  placeholder: { type: String, default: '' },
  id: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
  /** 用于解释每个值的语义，可选 */
  formatLabel: { type: Function, default: null },
  /** 标记某些值有特殊含义，可选 */
  valueHints: { type: Object, default: () => ({}) },
})

const emit = defineEmits(['update:modelValue', 'change'])

const open = ref(false)
const root = ref(null)
const panel = ref(null)

const numericValue = computed(() => {
  const n = Number(props.modelValue)
  return Number.isFinite(n) ? n : props.min
})

const cells = computed(() => {
  const list = []
  for (let v = props.min; v <= props.max; v++) list.push(v)
  return list
})

const displayText = computed(() => {
  if (props.formatLabel) return props.formatLabel(numericValue.value)
  return String(numericValue.value)
})

function toggle() {
  if (props.disabled) return
  open.value = !open.value
  if (open.value) {
    nextTick(() => {
      const el = panel.value?.querySelector('.gnp-cell.is-selected')
      el && el.scrollIntoView({ block: 'nearest' })
    })
  }
}

function close() {
  open.value = false
}

function select(v) {
  emit('update:modelValue', v)
  emit('change', v)
  close()
}

function onDocClick(e) {
  if (!open.value) return
  if (root.value && !root.value.contains(e.target)) close()
}

function onKeydown(e) {
  if (!open.value) {
    if (e.key === 'Enter' || e.key === ' ' || e.key === 'ArrowDown') {
      toggle()
      e.preventDefault()
    }
    return
  }
  const cols = Math.max(1, props.columns)
  const cur = numericValue.value
  let next = cur
  switch (e.key) {
    case 'Escape':
      close()
      e.preventDefault()
      return
    case 'Enter':
      select(cur)
      e.preventDefault()
      return
    case 'ArrowLeft':
      next = cur - 1
      break
    case 'ArrowRight':
      next = cur + 1
      break
    case 'ArrowUp':
      next = cur - cols
      break
    case 'ArrowDown':
      next = cur + cols
      break
    case 'Home':
      next = props.min
      break
    case 'End':
      next = props.max
      break
    default:
      return
  }
  if (next < props.min) next = props.min
  if (next > props.max) next = props.max
  emit('update:modelValue', next)
  e.preventDefault()
}

watch(open, (v) => {
  if (v) document.addEventListener('mousedown', onDocClick)
  else document.removeEventListener('mousedown', onDocClick)
})

onBeforeUnmount(() => {
  document.removeEventListener('mousedown', onDocClick)
})
</script>

<template>
  <div class="gnp-wrapper" ref="root" @keydown="onKeydown">
    <button
      type="button"
      class="form-select gnp-trigger"
      :id="id"
      :disabled="disabled"
      :aria-expanded="open"
      aria-haspopup="grid"
      @click="toggle"
    >
      <span class="gnp-value">{{ displayText }}</span>
      <span v-if="placeholder && !displayText" class="gnp-placeholder">{{ placeholder }}</span>
    </button>

    <Transition name="gnp-fade">
      <div v-if="open" ref="panel" class="gnp-panel" role="grid">
        <div class="gnp-header">
          <span class="gnp-range">{{ min }} – {{ max }}</span>
          <button type="button" class="gnp-clear" @click="select(min)">
            {{ String(min) }}
          </button>
        </div>
        <div
          class="gnp-grid"
          :style="{ gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))` }"
        >
          <button
            v-for="v in cells"
            :key="v"
            type="button"
            class="gnp-cell"
            :class="{
              'is-selected': v === numericValue,
              'is-hint': valueHints[v] != null,
            }"
            :title="valueHints[v] || formatLabel?.(v) || String(v)"
            @click="select(v)"
          >
            {{ v }}
          </button>
        </div>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.gnp-wrapper {
  position: relative;
  width: 100%;
}

.gnp-trigger {
  text-align: left;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.5rem;
}

.gnp-trigger:disabled {
  cursor: not-allowed;
  opacity: 0.65;
}

.gnp-value {
  font-variant-numeric: tabular-nums;
  font-weight: 500;
}

.gnp-placeholder {
  color: var(--bs-secondary-color, #6c757d);
}

.gnp-panel {
  position: absolute;
  top: calc(100% + 6px);
  left: 0;
  right: 0;
  z-index: 1050;
  padding: 0.6rem;
  border-radius: 0.5rem;
  border: 1px solid var(--bs-border-color, rgba(0, 0, 0, 0.12));
  background-color: var(--bs-body-bg, #fff);
  color: var(--bs-body-color, #212529);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.12);
}

.gnp-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.5rem;
  font-size: 0.8125rem;
  color: var(--bs-secondary-color, #6c757d);
}

.gnp-clear {
  border: 0;
  background: transparent;
  color: var(--bs-link-color, #0d6efd);
  font-size: 0.8125rem;
  padding: 0 0.25rem;
  cursor: pointer;
  border-radius: 0.25rem;
}

.gnp-clear:hover {
  background-color: var(--bs-tertiary-bg, rgba(0, 0, 0, 0.05));
}

.gnp-grid {
  display: grid;
  gap: 4px;
}

.gnp-cell {
  appearance: none;
  border: 1px solid transparent;
  background-color: var(--bs-tertiary-bg, rgba(0, 0, 0, 0.04));
  color: var(--bs-body-color, #212529);
  border-radius: 0.375rem;
  padding: 0.4rem 0;
  font-size: 0.875rem;
  font-variant-numeric: tabular-nums;
  cursor: pointer;
  transition: background-color 0.12s ease, border-color 0.12s ease, color 0.12s ease, transform 0.06s ease;
}

.gnp-cell:hover {
  background-color: var(--bs-secondary-bg, rgba(0, 0, 0, 0.08));
  border-color: var(--bs-border-color, rgba(0, 0, 0, 0.16));
}

.gnp-cell:active {
  transform: scale(0.97);
}

.gnp-cell.is-selected {
  background-color: var(--bs-primary, #0d6efd);
  border-color: var(--bs-primary, #0d6efd);
  color: #fff;
  font-weight: 600;
  box-shadow: 0 0 0 2px rgba(var(--bs-primary-rgb, 13, 110, 253), 0.2);
}

.gnp-cell.is-hint::after {
  content: '•';
  margin-left: 2px;
  color: var(--bs-warning, #ffc107);
}

/* dark theme polish */
:global([data-bs-theme='dark']) .gnp-panel {
  border-color: rgba(255, 255, 255, 0.1);
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.5);
}

:global([data-bs-theme='dark']) .gnp-cell {
  background-color: rgba(255, 255, 255, 0.05);
}

:global([data-bs-theme='dark']) .gnp-cell:hover {
  background-color: rgba(255, 255, 255, 0.1);
  border-color: rgba(255, 255, 255, 0.2);
}

.gnp-fade-enter-active,
.gnp-fade-leave-active {
  transition: opacity 0.12s ease, transform 0.12s ease;
}

.gnp-fade-enter-from,
.gnp-fade-leave-to {
  opacity: 0;
  transform: translateY(-4px);
}
</style>
