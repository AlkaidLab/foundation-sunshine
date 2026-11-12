<template>
  <MItem v-if="lockOf" icon="lock" :title="name" :text="lockOf" disabled />
  <MSwitchItem v-else-if="field.kind === 'toggle'" :model-value="!!value" :title="name" :text="currentText" :disabled="off" @update:model-value="set" />
  <MSliderItem v-else-if="field.kind === 'slider'" :model-value="Number(value)" size="m" :title="name" :text="currentText" :min="field.min" :max="field.max" :step="field.step" :decimals="field.decimals" :log="field.log" :unit="field.unit" :marks="field.marks" :disabled="off" @update:model-value="set" />
  <MItem v-else-if="seg" stack :disabled="off" :text="currentText || noteOf">
    <template #title>{{ name }}</template>
    <template #control><MSeg :model-value="value" block :disabled="off" :label="name" :options="field.options.map((o) => ({ value: o.value, label: labelOf(o) }))" @update:model-value="set" /></template>
  </MItem>
  <MFieldItem v-else-if="field.kind === 'text'" :model-value="String(value ?? '')" :label="name" :disabled="off" @update:model-value="set" />
  <MChoiceItem v-else :model-value="value" :title="name" :options="choices" :disabled="off" :cancel-label="cancelLabel" @update:model-value="set" />
</template>

<script setup>
import { computed } from 'vue'
import { MChoiceItem, MFieldItem, MItem, MSeg, MSliderItem, MSwitchItem } from 'foundation-ui/vue'
import { segmentsFit } from '../../utils/sparxie/gameFields.js'

// The control of one described item (kind: choice | toggle | slider). `enabledBy`: off while another toggle is off. `locks[key]`: taken over by something else, shown off with that text.
const props = defineProps({
  field: { type: Object, required: true },
  values: { type: Object, required: true },
  defaults: { type: Function, required: true },
  names: { type: Object, default: () => ({}) },
  optionName: { type: Function, default: null },
  current: { type: Object, default: null },
  words: { type: Object, default: () => ({ current: '', on: '', off: '' }) },
  disabled: { type: Boolean, default: false },
  locks: { type: Object, default: () => ({}) },
  /** key -> value -> the small line under a choice whose current option needs one (what 自动 means) */
  notes: { type: Object, default: () => ({}) },
  cancelLabel: { type: String, default: 'Cancel' },
  zh: { type: Boolean, default: true },
})
const f = computed(() => props.field)
const effective = (k) => props.values[k] ?? props.defaults(k, props.field)
const value = computed(() => effective(props.field.key))
const set = (v) => { props.values[props.field.key] = v }
const lockOf = computed(() => props.locks[props.field.key] || '')
const off = computed(() => props.disabled || (!!props.field.enabledBy && !effective(props.field.enabledBy)))
const name = computed(() => props.names[props.field.key] || (props.zh ? f.value.label || f.value.labelEn : f.value.labelEn || f.value.label) || props.field.key)
const labelOf = (o) => (props.zh ? o.label || o.labelEn : o.labelEn || o.label) || (props.optionName ? props.optionName(props.field, o.value) : '') || String(o.value)
const seg = computed(() => props.field.kind === 'choice' && segmentsFit(props.field.options.map(labelOf)))
const noteOf = computed(() => props.notes[props.field.key]?.[value.value] || '')
const currentValue = computed(() => (props.current && props.current[props.field.key] !== undefined && props.current[props.field.key] !== null ? props.current[props.field.key] : undefined))
const currentText = computed(() => {
  const v = currentValue.value
  if (v === undefined || !props.words.current) return ''
  if (props.field.kind === 'toggle') return `${props.words.current} ${v ? props.words.on : props.words.off}`
  const o = props.field.kind === 'choice' ? props.field.options.find((x) => x.value === v) : null
  const shown = props.field.kind === 'slider' && typeof v === 'number' ? v.toFixed(props.field.decimals ?? 0) : String(v)
  return `${props.words.current} ${o ? labelOf(o) : shown}${props.field.unit || ''}`
})
const choices = computed(() => (props.field.options || []).map((o) => ({ value: o.value, label: labelOf(o), text: currentValue.value === o.value ? props.words.current : '' })))
</script>
