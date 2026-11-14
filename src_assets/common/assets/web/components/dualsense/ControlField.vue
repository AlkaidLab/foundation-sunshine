<!--
  Range and colour controls follow the WHATWG HTML Living Standard input
  states (ds5-whatwg-input-20261031). The slider keeps its slider role, value text
  and keyboard steps from the shared component (WAI-ARIA APG Slider Pattern,
  ds5-wai-aria-slider-20261031); a double click on it restores the default.
-->
<template>
  <MChoiceItem
    v-if="kind === 'select'"
    :model-value="selectedOption"
    :title="fieldLabel"
    :options="selectOptions"
    :disabled="disabled"
    :cancel-label="$t('_common.cancel')"
    :data-ds5-key="descriptor.id"
    :data-cfg="cfgId"
    @update:model-value="selectValue"
  >
    <template v-if="tags || reset" #trail><MChip v-if="descriptor.expert" tone="warn">{{ $t('ds5.expert') }}</MChip><MChip v-if="descriptor.live === 4" :data-tip="$t('ds5.next_connection_hint')">{{ $t('ds5.next_connection') }}</MChip><MIconButton v-if="reset" icon="restart_alt" :label="$t('ds5.reset_key')" @click.stop="$emit('reset')" /></template>
  </MChoiceItem>

  <MItem v-else :title="fieldLabel" :disabled="disabled" :stack="stacked" class="ds5-field" :class="{ 'is-modified': modified }" :data-ds5-key="descriptor.id" :data-cfg="cfgId" :data-tip="disabled ? $t('ds5.pending.reason') : undefined">
    <template #trail>
      <MChip v-if="descriptor.expert" tone="warn">{{ $t('ds5.expert') }}</MChip>
      <MChip v-if="descriptor.live === 4" :data-tip="$t('ds5.next_connection_hint')">{{ $t('ds5.next_connection') }}</MChip>
      <span v-if="kind === 'slider'" class="ds5-readout">{{ displayValue }}</span>
      <MIconButton v-if="reset" icon="restart_alt" :label="$t('ds5.reset_key')" @click="$emit('reset')" />
      <MSwitch v-if="kind === 'toggle'" :model-value="modelValue === true" :label="fieldLabel" :disabled="disabled" @update:model-value="$emit('update:modelValue', $event)" />
    </template>
    <template v-if="stacked" #control>
      <MSlider
        v-if="kind === 'slider'"
        :model-value="Number(modelValue)"
        :min="Number(descriptor.min)"
        :max="Number(descriptor.max)"
        :step="sliderStep"
        :decimals="decimals"
        :unit="unitSuffix"
        :log="descriptor.scale === 'log'"
        :centered="centered"
        :center="centered ? 0 : null"
        size="m"
        :icon-start="ends[0]"
        :icon-end="ends[1]"
        :disabled="disabled"
        :label="fieldLabel"
        @update:model-value="onSlider"
        @dblclick="$emit('reset')"
      />
      <MSeg v-else-if="kind === 'segmented'" block :label="fieldLabel" :model-value="selectedOption" :options="segOptions" :disabled="disabled" @update:model-value="selectValue" />
      <CurvePreview v-else-if="kind === 'curve'" :model-value="String(modelValue)" :label="fieldLabel" :disabled="disabled" @update:model-value="$emit('update:modelValue', $event)" />
      <MSeg v-else-if="kind === 'chips'" multiple block :label="fieldLabel" :model-value="chipValues" :options="segOptions" :disabled="disabled" @update:model-value="setChips" />
    </template>
  </MItem>
</template>

<script setup>
import { MChip, MChoiceItem, MIconButton, MItem, MSeg, MSlider, MSwitch } from 'foundation-ui/vue'
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import CurvePreview from './CurvePreview.vue'
import { localizedKeyLabel, localizedOptionLabel } from '../../config/dualsenseI18n.js'
import { sliderEnds } from './tuningIcons.js'

const props = defineProps({
  descriptor: { type: Object, required: true },
  modelValue: { type: [Boolean, Number, String], required: true },
  disabled: { type: Boolean, default: false },
})

const emit = defineEmits(['update:modelValue', 'reset'])
const { locale } = useI18n()

const fieldLabel = computed(() => localizedKeyLabel(props.descriptor, locale.value))
const isInteger = computed(() => props.descriptor.type === 'u32' || props.descriptor.type === 'i32')
const numeric = computed(() => ['f32', 'u32', 'i32'].includes(props.descriptor.type))

const kind = computed(() => {
  switch (props.descriptor.control) {
    case 'slider':
    case 'color_channel':
      return 'slider'
    case 'toggle':
      return 'toggle'
    case 'segmented':
      return fitsSegmented.value ? 'segmented' : 'select'
    case 'curve':
      return 'curve'
    case 'chip_multiselect':
      return 'chips'
    default:
      return 'select'
  }
})
// A connected button group only stays readable while its labels are short; wide CJK
// characters count in full, Latin letters about half.
const fitsSegmented = computed(() => {
  const names = props.descriptor.enum_names || []
  if (names.length > 4) return false
  const width = names
    .map(name => localizedOptionLabel(name, locale.value))
    .join('')
    .split('')
    .reduce((sum, character) => sum + (character.charCodeAt(0) > 0x2e80 ? 1 : 0.55), 0)
  return width <= 13
})
const stacked = computed(() => ['slider', 'segmented', 'curve', 'chips'].includes(kind.value))

const decimals = computed(() => {
  const step = Number(props.descriptor.step)
  if (isInteger.value || !Number.isFinite(step) || step >= 1 || step <= 0) return 0
  return Math.min(6, Math.max(0, Math.ceil(-Math.log10(step))))
})
const unitSuffix = computed(() => {
  if (props.descriptor.unit === 'deg') return '°'
  return props.descriptor.unit ? ' ' + props.descriptor.unit : ''
})
const displayValue = computed(() => {
  const number = Number(props.modelValue)
  return (numeric.value && Number.isFinite(number) ? number.toFixed(decimals.value) : String(props.modelValue)) + unitSuffix.value
})
const sliderStep = computed(() => {
  const step = Number(props.descriptor.step)
  if (step > 0) return step
  return isInteger.value ? 1 : 0.01
})
// A value around zero (an offset) is drawn from the middle outwards
const centered = computed(() => Number(props.descriptor.min) < 0 && Number(props.descriptor.max) > 0)
const ends = computed(() => sliderEnds(props.descriptor.key))
// the id a search hit lands on
const cfgId = computed(() => `ds5:${props.descriptor.key}`)
const onSlider = (value) => emit('update:modelValue', isInteger.value ? Math.round(Number(value)) : Number(value))

const same = (left, right) => {
  if (numeric.value) return Math.abs(Number(left) - Number(right)) < 1e-9
  return String(left) === String(right)
}
const modified = computed(() => !same(props.modelValue, props.descriptor.default))
const reset = computed(() => modified.value && !props.disabled)
const tags = computed(() => props.descriptor.expert || props.descriptor.live === 4)

const optionLabel = (option) => localizedOptionLabel(option, locale.value)
const segOptions = computed(() => (props.descriptor.enum_names || []).map(name => ({ value: name, label: optionLabel(name) })))
const selectOptions = segOptions
const selectedOption = computed(() => {
  if (!isInteger.value) return String(props.modelValue)
  return Number(props.modelValue) === 0 && (props.descriptor.enum_names || []).includes('auto') ? 'auto' : String(props.modelValue)
})
const selectValue = (option) => {
  if (isInteger.value) emit('update:modelValue', option === 'auto' ? 0 : Number(option))
  else emit('update:modelValue', option)
}

// A multi-select value is a bit mask: one bit per option name
const chipValues = computed(() => (props.descriptor.enum_names || []).filter((name, index) => (Number(props.modelValue) & (1 << index)) !== 0))
const setChips = (names) => {
  const all = props.descriptor.enum_names || []
  emit('update:modelValue', names.reduce((mask, name) => mask | (1 << all.indexOf(name)), 0))
}
</script>

<style>
.ds5-readout { font: var(--m-label-large); font-variant-numeric: tabular-nums; color: var(--md-sys-color-primary-text); text-align: right; }
.ds5-field.is-modified .m-item__title::after { content: ''; display: inline-block; width: 6px; height: 6px; margin-inline-start: 8px; border-radius: 50%; background: var(--md-sys-color-primary); vertical-align: middle; }
</style>
