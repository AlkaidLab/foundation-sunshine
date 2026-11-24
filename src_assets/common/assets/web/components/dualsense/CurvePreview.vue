<template>
  <div class="ds5-curve">
    <div class="ds5-curve-options" role="radiogroup" :aria-label="label">
      <button
        v-for="option in curves"
        :key="option"
        type="button"
        class="ds5-curve-option"
        :class="{ selected: option === modelValue }"
        role="radio"
        :aria-checked="option === modelValue"
        :disabled="disabled"
        @click="$emit('update:modelValue', option)"
      >
        {{ $t('ds5.curve.' + option) }}
      </button>
    </div>
    <svg class="ds5-curve-chart" viewBox="0 0 220 120" role="img" :aria-label="label">
      <g class="ds5-curve-grid"><path d="M10 10H210M10 40H210M10 70H210M10 100H210" /></g>
      <path class="ds5-curve-ghost" d="M10 100L210 10" />
      <path class="ds5-curve-area" :d="paths.area" transform="translate(10 10)" />
      <path class="ds5-curve-line" :d="paths.line" transform="translate(10 10)" />
      <circle class="ds5-curve-end" cx="210" cy="10" r="4" />
      <text class="ds5-curve-tick" x="10" y="116">0</text><text class="ds5-curve-tick" x="210" y="116" text-anchor="end">1</text>
      <text class="ds5-curve-tick" x="2" y="104">0</text><text class="ds5-curve-tick" x="2" y="14">1</text>
    </svg>
    <div class="ds5-curve-axis"><span>0</span><span>1</span></div>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { curvePaths } from 'foundation-ui/vue'

const props = defineProps({
  modelValue: { type: String, default: 'linear' },
  label: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
})

defineEmits(['update:modelValue'])

const curves = ['linear', 'enhanced', 'quadratic', 'cubic', 'easeOutQuad', 'easeOutCubic']

const shape = (curve, value) => {
  const m = Math.min(1, Math.max(0, value))
  switch (curve) {
    case 'enhanced':
      return m * (0.5 + 0.5 * m)
    case 'quadratic':
      return m * m
    case 'cubic':
      return m * m * m
    case 'easeOutQuad':
      return 1 - (1 - m) * (1 - m)
    case 'easeOutCubic':
      return 1 - Math.pow(1 - m, 3)
    case 'linear':
    default:
      return m
  }
}

// the curve as a smooth path over the real mapping, scaled into the 200 x 90 plot area
const paths = computed(() => curvePaths(Array.from({ length: 33 }, (_, i) => shape(props.modelValue, i / 32)), { w: 200, h: 90, max: 1, pad: 0 }))
</script>

<style scoped>
.ds5-curve {
  display: grid;
  gap: 0.5rem;
  width: 100%;
}

.ds5-curve-options {
  display: inline-flex;
  flex-wrap: wrap;
  align-self: start;
  gap: 2px;
  padding: 2px;
  border-radius: var(--ds5-control-radius);
  background: var(--ds5-segment-bg);
  width: fit-content;
  max-width: 100%;
}

.ds5-curve-option {
  height: 1.625rem;
  padding: 0 0.625rem;
  border: 0;
  border-radius: calc(var(--ds5-control-radius) - 2px);
  background: transparent;
  color: var(--ds5-muted);
  font-size: 0.75rem;
  font-weight: 500;
}

.ds5-curve-option.selected {
  background: var(--ds5-segment-on-bg);
  color: var(--ds5-segment-on-text);
  box-shadow: var(--ds5-segment-on-shadow);
}

.ds5-curve-option:focus-visible {
  outline: 2px solid var(--ds5-focus);
  outline-offset: 2px;
}

.ds5-curve-option:disabled {
  opacity: 0.6;
}

.ds5-curve-chart {
  width: 100%;
  max-width: 22rem;
  height: auto;
  border-radius: 1.25rem;
  background: var(--md-sys-color-surface-container);
}

.ds5-curve-grid {
  fill: none;
  stroke: color-mix(in srgb, var(--md-sys-color-on-surface) 12%, transparent);
  stroke-width: 1;
  stroke-dasharray: 2 4;
}

.ds5-curve-ghost {
  fill: none;
  stroke: var(--md-sys-color-on-surface-variant);
  stroke-dasharray: 3 3;
  stroke-width: 1;
  opacity: 0.5;
}

.ds5-curve-line {
  fill: none;
  stroke: var(--md-sys-color-primary);
  stroke-linecap: round;
  stroke-width: 3;
}

.ds5-curve-area {
  fill: color-mix(in srgb, var(--md-sys-color-primary) 16%, transparent);
}

.ds5-curve-tick {
  fill: var(--md-sys-color-on-surface-variant);
  font-size: 9px;
}

.ds5-curve-end {
  fill: var(--md-sys-color-primary);
}

.ds5-curve-axis {
  display: none;
}
</style>
