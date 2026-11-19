<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useCfgText } from '../../composables/useCfgText.js'
import { useCfgPin } from '../../composables/useCfgPin.js'
import { MButton, MExpertItem, MPickSheet, MSeg, MSlider, MSwitch, MValueDialog } from 'foundation-ui/vue'
import { browsePath } from '../../utils/browsePath.js'
import { SETTING_META, constraintText, isChanged, validateSetting } from 'foundation-ui/settings/index.js'
import { stepIndex } from './cfgValue.js'
import { useOptionLabels } from '../../composables/optionLabels.js'
import { labelPx, segFits, useRowWidth } from '../../composables/useSegFit.js'

// A tier 3 (expert) setting: its control works in place and the row opens to say what it does, what it changes, the
// default and a reset. One component for every control type, driven by the setting's entry in the registry
// (`SETTING_META`): switch, button group, sheet picker, slider (numbers or named steps), field. Text keys: the title is
// `config.<k>`, the explanation `config.<k>_desc`, the effect `config.effect.<k>`.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  /** overrides the control of the registry */
  control: { type: String, default: '' },
  /** [{ value, label }] for a button group or a picker */
  options: { type: Array, default: () => [] },
  on: { type: [String, Boolean], default: 'enabled' },
  off: { type: [String, Boolean], default: 'disabled' },
  /** overrides the default of the registry */
  def: { type: [String, Number, Boolean], default: undefined },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  tp: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  type: { type: String, default: 'text' },
  placeholder: { type: String, default: '' },
  min: { type: Number, default: undefined },
  max: { type: Number, default: undefined },
  step: { type: Number, default: undefined },
  unit: { type: String, default: undefined },
  decimals: { type: Number, default: undefined },
  steps: { type: Array, default: undefined },
  prefix: { type: String, default: undefined },
  /** text for the numeric sentinel shown at the slider's lowest position */
  offText: { type: String, default: undefined },
})
const { t, title, info } = useCfgText(props)
const pin = useCfgPin(props, title)
useOptionLabels(props)
const { te, locale } = useI18n()
const meta = computed(() => SETTING_META[props.k] || {})
const kind = computed(() => props.control || meta.value.control || 'field')
const defValue = computed(() => (props.def !== undefined ? props.def : meta.value.def))
const stored = computed(() => props.config[props.k])
const effect = computed(() => (te(`config.effect.${props.k}`) ? t(`config.effect.${props.k}`) : ''))
const texts = computed(() => ({
  changed: t('config.expert_changed'), risk: t('config.expert_risk'), desc: t('config.expert_desc'), effect: t('config.expert_effect'),
  default: t('config.expert_default'), reset: t('config.expert_reset'),
}))

const stepList = computed(() => props.steps ?? meta.value.steps)
const lo = computed(() => props.min ?? meta.value.min ?? 0)
const hi = computed(() => props.max ?? meta.value.max ?? 100)
const unit = computed(() => props.unit ?? meta.value.unit ?? '')
const decimals = computed(() => props.decimals ?? meta.value.decimals ?? 0)
const risk = computed(() => meta.value.risk || '')

// the default of a switch as a yes or no: the registry has a boolean, a template may give the stored form ('enabled')
const defBool = computed(() => {
  const d = defValue.value
  if (d === undefined || d === null) return false
  if (typeof d === 'boolean') return d
  return String(d) === String(props.on) || ['true', 'enabled', 'on'].includes(String(d))
})
const bool = computed({
  get: () => String(stored.value ?? (defBool.value ? props.on : props.off)) === String(props.on),
  set: (value) => { props.config[props.k] = value ? props.on : props.off },
})
// an option may name other spellings the host accepts (aliases), so a configuration written by hand still shows its value
const choice = computed({
  get: () => {
    const current = String(stored.value ?? defValue.value)
    const option = props.options.find((o) => String(o.value) === current) ?? props.options.find((o) => (o.aliases || []).some((alias) => String(alias) === current))
    return option?.value ?? ''
  },
  set: (value) => { props.config[props.k] = value },
})
// a number with a value that means "off" or "automatic" (-1, 0): the lowest position of the slider stands for it
const offValue = computed(() => meta.value.off)
const offText = computed(() => props.offText ?? meta.value.offText)
const number = computed({
  get: () => {
    if (stepList.value) return stepIndex(stepList.value, stored.value, defValue.value)
    const n = Number(stored.value ?? defValue.value ?? lo.value)
    if (offValue.value !== undefined && n === offValue.value) return lo.value
    return Number.isFinite(n) ? n : lo.value
  },
  set: (value) => { props.config[props.k] = stepList.value ? stepList.value[value] : (offValue.value !== undefined && value <= lo.value ? (meta.value.offWrite !== undefined ? meta.value.offWrite : offValue.value) : value) },
})
const text = computed({
  get: () => (stored.value === undefined || stored.value === null ? '' : stored.value),
  set: (value) => { props.config[props.k] = value },
})

const short = (label) => String(label).split(/\s*[（(]/)[0]
const optionLabel = (value) => short(props.options.find((o) => String(o.value) === String(value))?.label ?? '')
const stepLabel = (index) => `${props.prefix ?? meta.value.prefix ?? ''}${stepList.value[index]}`
const fmt = (n) => (offValue.value !== undefined && n <= lo.value ? t(offText.value === 'auto' ? '_common.auto' : '_common.disabled') : `${Number(n).toFixed(decimals.value)}${unit.value}`)
const valueText = computed(() => {
  switch (kind.value) {
    case 'switch': return bool.value ? t('_common.enabled') : t('_common.disabled')
    case 'seg': case 'sheet': return optionLabel(choice.value)
    case 'slider': return stepList.value ? stepLabel(number.value) : fmt(number.value)
    default: return String(text.value || defValue.value || '') || t('_common.auto')
  }
})
const defText = computed(() => {
  const d = defValue.value
  switch (kind.value) {
    case 'switch': return defBool.value ? t('_common.enabled') : t('_common.disabled')
    case 'seg': case 'sheet': return optionLabel(d) || t('_common.auto')
    case 'slider': return stepList.value ? stepLabel(stepIndex(stepList.value, d, d)) : fmt(Number(d ?? lo.value))
    default: return d === null || d === undefined || d === '' ? t('_common.auto') : String(d)
  }
})
const changed = computed(() => isChanged(props.k, stored.value, defValue.value))
const reset = () => {
  const d = defValue.value
  if (kind.value === 'switch') props.config[props.k] = defBool.value ? props.on : props.off
  else if (d === null || d === undefined) props.config[props.k] = ''
  else props.config[props.k] = d
}
// a button group suits two to four short names; anything else is the sheet picker
const width = (s) => [...s].reduce((sum, ch) => sum + (ch.charCodeAt(0) > 0x2e80 ? 2 : 1), 0)
// the row's own width decides: names that would be cut off, or buttons that would not be a finger wide, are the value row that opens the list
const rowWidth = useRowWidth(() => 32)
const asSeg = computed(() => kind.value === 'seg' && props.options.length >= 2 && props.options.length <= 4 && props.options.every((o) => width(short(o.label)) <= 18) && segFits(props.options.map((o) => short(o.label)), rowWidth.value))
const pickerOpen = ref(false)
const browse = computed(() => (kind.value === 'path' ? browsePath(props.type === 'folder' ? 'folder' : 'file', () => title.value) : null))
const fieldOpen = ref(false)
const fieldText = computed(() => String(text.value || defValue.value || '') || t('_common.auto'))
const check = (value) => constraintText(String(locale.value), validateSetting(props.k, value))
// a value button beside the title needs room: the title keeps 112 px, the info icon and the pin menu 100 px, the button its name; a row too narrow for all puts the button under the title, the width of the row
const sheetLike = computed(() => kind.value === 'sheet' || (kind.value === 'seg' && !asSeg.value))
const buttonBeside = computed(() => rowWidth.value <= 0 || rowWidth.value - 112 - 100 - 12 >= labelPx(optionLabel(choice.value) || t('_common.auto')) + 32)
const stack = computed(() => kind.value !== 'switch' && (sheetLike.value ? !buttonBeside.value : true))
const segOptions = computed(() => props.options.map((o) => ({ value: o.value, label: short(o.label) })))
</script>

<template>
  <MExpertItem :data-cfg="k" :title="title" :value="valueText" :changed="changed" :risk="risk" :desc="info" :def-text="defText" :effect="effect" :stack="stack" :disabled="disabled" :texts="texts" :pin="pin" @reset="reset">
    <MSwitch v-if="kind === 'switch'" v-model="bool" :label="title" :disabled="disabled" />
    <MSeg v-else-if="asSeg" v-model="choice" block :label="title" :options="segOptions" :disabled="disabled" />
    <template v-else-if="kind === 'sheet' || kind === 'seg'">
      <MButton variant="tonal" size="sm" :disabled="disabled" @click="pickerOpen = true">{{ optionLabel(choice) || t('_common.auto') }}</MButton>
      <MPickSheet v-model="choice" v-model:open="pickerOpen" :title="title" :options="options" :search-label="t('_common.search')" :empty-text="t('_common.no_results')" :close-label="t('_common.close')" />
    </template>
    <MSlider v-else-if="kind === 'slider'" v-model="number" :min="stepList ? 0 : lo" :max="stepList ? stepList.length - 1 : hi" :step="stepList ? 1 : (props.step ?? meta.step ?? 1)" :decimals="decimals" :unit="unit" size="m" :label="title" :disabled="disabled" :off-text="offValue !== undefined && !stepList ? fmt(lo) : ''" />
    <template v-else>
      <MButton variant="tonal" size="sm" :disabled="disabled" @click="fieldOpen = true">{{ fieldText }}</MButton>
      <MValueDialog v-model="text" v-model:open="fieldOpen" :title="title" :type="kind === 'path' ? 'text' : type" :placeholder="placeholder" :help="info" :check="check" :ok-label="t('_common.ok')" :cancel-label="t('_common.cancel')" :browse="kind === 'path' ? browse : null" :browse-label="t('_common.browse')" :browse-icon="type === 'folder' ? 'folder_open' : 'upload_file'" />
    </template>
  </MExpertItem>
</template>
