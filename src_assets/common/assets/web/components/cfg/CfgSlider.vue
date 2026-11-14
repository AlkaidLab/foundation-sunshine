<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MSliderItem } from 'foundation-ui/vue'
import { SETTING_META } from 'foundation-ui/settings/index.js'
import { useCfgPin } from '../../composables/useCfgPin.js'
import { stepIndex } from './cfgValue.js'

// One number (or one named step) of the configuration as the Expressive slider with its value indicator. Tier 2.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  tp: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  icon: { type: String, default: '' },
  iconStart: { type: String, default: '' },
  iconEnd: { type: String, default: '' },
  min: { type: Number, default: undefined },
  max: { type: Number, default: undefined },
  step: { type: Number, default: undefined },
  unit: { type: String, default: undefined },
  decimals: { type: Number, default: undefined },
  steps: { type: Array, default: undefined },
  prefix: { type: String, default: undefined },
  /** text of each named step, in the order of `steps` */
  labels: { type: Array, default: undefined },
  def: { type: [String, Number], default: undefined },
})
const { title, info } = useCfgText(props)
const pin = useCfgPin(props, title)
const meta = computed(() => SETTING_META[props.k] || {})
const stepList = computed(() => props.steps ?? meta.value.steps)
const defValue = computed(() => (props.def !== undefined ? props.def : meta.value.def))
const lo = computed(() => props.min ?? meta.value.min ?? 0)
const hi = computed(() => props.max ?? meta.value.max ?? 100)
const model = computed({
  get: () => {
    if (stepList.value) return stepIndex(stepList.value, props.config[props.k], defValue.value)
    const n = Number(props.config[props.k] ?? defValue.value ?? lo.value)
    return Number.isFinite(n) ? n : lo.value
  },
  set: (value) => { props.config[props.k] = stepList.value ? stepList.value[value] : value },
})
const readout = computed(() => (stepList.value ? (props.labels?.[model.value] ?? `${props.prefix ?? meta.value.prefix ?? ''}${stepList.value[model.value]}`) : ''))
</script>

<template>
  <MSliderItem
    v-model="model"
    :data-cfg="k"
    :title="title"
    :icon="icon"
    :info="info"
    :min="stepList ? 0 : lo"
    :max="stepList ? stepList.length - 1 : hi"
    :step="stepList ? 1 : (step ?? meta.step ?? 1)"
    :decimals="decimals ?? meta.decimals ?? 0"
    :unit="unit ?? meta.unit ?? ''"
    :readout-text="readout"
    :icon-start="iconStart"
    :icon-end="iconEnd"
    size="m"
    :disabled="disabled"
    :pin="pin"
  />
</template>
