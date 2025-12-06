<template>
  <svg
    xmlns="http://www.w3.org/2000/svg"
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    :stroke-width="strokeWidth"
    stroke-linecap="round"
    stroke-linejoin="round"
    :class="iconClass"
  >
    <component :is="iconPaths[name]" />
  </svg>
</template>

<script setup>
import { computed, h } from 'vue'

const props = defineProps({
  name: {
    type: String,
    required: true,
    validator: (value) => ['lock', 'clock', 'star'].includes(value),
  },
  size: {
    type: [String, Number],
    default: 24,
  },
  strokeWidth: {
    type: [String, Number],
    default: 2,
  },
  iconClass: {
    type: String,
    default: '',
  },
})

const iconPaths = {
  lock: {
    render: () => [
      h('rect', { x: 3, y: 11, width: 18, height: 11, rx: 2, ry: 2 }),
      h('path', { d: 'M7 11V7a5 5 0 0 1 10 0v4' }),
    ],
  },
  clock: {
    render: () => [
      h('circle', { cx: 12, cy: 12, r: 10 }),
      h('polyline', { points: '12 6 12 12 16 14' }),
    ],
  },
  star: {
    render: () => [
      h('path', {
        d: 'M12 3l1.912 5.813a2 2 0 0 0 1.275 1.275L21 12l-5.813 1.912a2 2 0 0 0-1.275 1.275L12 21l-1.912-5.813a2 2 0 0 0-1.275-1.275L3 12l5.813-1.912a2 2 0 0 0 1.275-1.275L12 3z',
      }),
    ],
  },
}
</script>

<style scoped>
svg {
  display: inline-block;
  vertical-align: middle;
}
</style>
