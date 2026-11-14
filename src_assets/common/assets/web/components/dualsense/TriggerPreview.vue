<template>
  <div class="ds5-trigger-preview">
    <MSeg block :label="$t('ds5.trigger_preview.mode')" :model-value="selectedMode" :options="modes" @update:model-value="selectedMode = $event" />
    <div class="ds5-trigger-charts">
      <figure v-for="side in sides" :key="side.id" class="ds5-trigger-chart">
        <MDsPlot kind="series" :points="points(side.strength)" :x-label="$t('ds5.plots.travel')" :label="side.label" />
        <figcaption>{{ side.label }}</figcaption>
      </figure>
    </div>
    <p class="ds5-preview-note">
      <MIcon name="visibility" />
      {{ $t('ds5.trigger_preview.note') }}
    </p>
  </div>
</template>

<script setup>
import { MDsPlot, MIcon, MSeg } from 'foundation-ui/vue'
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'

// What each adaptive trigger mode does over the travel of the trigger, scaled by the strength values on the page. A preview
// of the shape: nothing is sent to the pad.
const props = defineProps({
  enabled: { type: Boolean, default: true },
  strength: { type: Number, default: 1 },
  leftStrength: { type: Number, default: 1 },
  rightStrength: { type: Number, default: 1 },
})

const { t } = useI18n()
const selectedMode = ref('feedback')
const modes = computed(() => ['feedback', 'weapon', 'vibration', 'slope'].map((mode) => ({ value: mode, label: t(`ds5.trigger_preview.${mode}`) })))
const sides = computed(() => [
  { id: 'left', label: t('ds5.test_panel.trigger_left'), strength: props.leftStrength },
  { id: 'right', label: t('ds5.test_panel.trigger_right'), strength: props.rightStrength },
])

const forceAt = (mode, position, strength) => {
  if (!props.enabled) return 0
  const scale = Math.max(0, Math.min(2, props.strength * strength))
  switch (mode) {
    case 'weapon':
      return position < 0.18 ? scale * 0.85 : 0.04
    case 'vibration':
      return 0.5 + 0.45 * scale * Math.sin(position * Math.PI * 14)
    case 'slope':
      return Math.max(0, Math.min(1, (position - 0.15) / 0.7)) * scale
    case 'feedback':
    default:
      if (position < 0.2) return 0
      if (position > 0.8) return 0
      return Math.min(1, scale * 0.9)
  }
}
const points = (sideStrength) => Array.from({ length: 65 }, (_, index) => forceAt(selectedMode.value, index / 64, sideStrength))
</script>

<style>
.ds5-trigger-preview { display: grid; gap: 12px; width: 100%; }
.ds5-trigger-charts { display: flex; flex-wrap: wrap; justify-content: center; gap: 12px 28px; }
.ds5-trigger-chart { margin: 0; flex: 1 1 260px; max-width: 360px; }
.ds5-trigger-chart figcaption { margin-top: 4px; text-align: center; font: var(--m-label-medium); color: var(--md-sys-color-on-surface-variant); }
.ds5-preview-note { margin: 0; display: flex; align-items: center; gap: 6px; font: var(--m-label-medium); color: var(--md-sys-color-on-surface-variant); }
</style>
