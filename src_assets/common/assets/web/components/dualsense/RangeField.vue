<template>
  <MItem :title="lowLabel" :text="highLabel" :disabled="disabled" stack class="ds5-field" :class="{ 'is-modified': modified }" :data-ds5-key="low.id" :data-cfg="`ds5:${low.key}`" :data-tip="disabled ? $t('ds5.pending.reason') : undefined">
    <template #trail>
      <i class="ds5-anchor" :data-ds5-key="high.id" :data-cfg="`ds5:${high.key}`" aria-hidden="true" />
      <span class="ds5-readout">{{ fmt(lowValue) }} – {{ fmt(highValue) }}</span>
      <MIconButton v-if="modified && !disabled" icon="restart_alt" :label="$t('ds5.reset_key')" @click="$emit('reset')" />
    </template>
    <template #control>
      <MSlider
        :model-value="[lowValue, highValue]"
        :min="min"
        :max="max"
        :step="step"
        :decimals="decimals"
        size="m"
        :disabled="disabled"
        :label="lowLabel"
        @update:model-value="onRange"
        @dblclick="$emit('reset')"
      />
    </template>
  </MItem>
</template>

<script setup>
// Two keys that bound one range (inner and outer dead zone, travel start and end) on one range slider.
import { MIconButton, MItem, MSlider } from 'foundation-ui/vue'
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { localizedKeyLabel } from '../../config/dualsenseI18n.js'

const props = defineProps({
  low: { type: Object, required: true },
  high: { type: Object, required: true },
  lowValue: { type: Number, required: true },
  highValue: { type: Number, required: true },
  disabled: { type: Boolean, default: false },
})
// one event per end, so the page stores each key on its own
const emit = defineEmits(['update:low', 'update:high', 'reset'])
const { locale } = useI18n()

const lowLabel = computed(() => localizedKeyLabel(props.low, locale.value))
const highLabel = computed(() => localizedKeyLabel(props.high, locale.value))
const min = computed(() => Math.min(Number(props.low.min), Number(props.high.min)))
const max = computed(() => Math.max(Number(props.low.max), Number(props.high.max)))
const step = computed(() => Number(props.low.step) > 0 ? Number(props.low.step) : 0.01)
const decimals = computed(() => (step.value >= 1 ? 0 : Math.min(6, Math.ceil(-Math.log10(step.value)))))
const fmt = (value) => Number(value).toFixed(decimals.value)
const round = (value) => Math.round(value / step.value) * step.value

const modified = computed(() => Math.abs(props.lowValue - Number(props.low.default)) > 1e-9 || Math.abs(props.highValue - Number(props.high.default)) > 1e-9)

const onRange = ([low, high]) => {
  const nextLow = Math.min(Math.max(round(low), Number(props.low.min)), Number(props.low.max))
  const nextHigh = Math.min(Math.max(round(high), Number(props.high.min)), Number(props.high.max))
  if (nextLow !== props.lowValue) emit('update:low', nextLow)
  if (nextHigh !== props.highValue) emit('update:high', nextHigh)
}
</script>

<style>
/* the outer end of the range has its own address for the search: a point inside the row of the pair */
.ds5-anchor { display: inline-block; width: 0; height: 0; }
</style>
