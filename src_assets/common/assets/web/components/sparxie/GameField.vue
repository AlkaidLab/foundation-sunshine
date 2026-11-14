<template>
  <!-- taken over by something else (the game's frame rate while Sparxie unlocks it): the item stays, off, and says by whom -->
  <div v-if="lockOf" class="gs-tile gs-tile--lock">
    <MIcon name="lock" />
    <span class="gs-tile__body"><span class="gs-tile__name">{{ name }}</span><small class="gs-hint">{{ lockOf }}</small></span>
  </div>

  <div v-else-if="control === 'switch'" class="gs-tile gs-tile--switch" :class="{ 'is-off': off }" @click="flip">
    <span class="gs-tile__body">
      <span class="gs-tile__name">{{ name }}<GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></span>
      <small v-if="shownHint" class="gs-hint">{{ shownHint }}</small>
    </span>
    <MSwitch :model-value="!!value" :label="name" :disabled="off" @update:model-value="set" />
  </div>

  <div v-else-if="control === 'segments'" class="gs-seg" :class="{ 'is-off': off, 'is-wide': wide }">
    <span class="gs-lab-row"><span class="gs-lab">{{ name }}</span><GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></span>
    <MSeg :model-value="value" block :disabled="off" :label="name" :options="options.map((o) => ({ value: o.value, label: o.label }))" @update:model-value="set" />
    <small v-if="shownHint" class="gs-hint">{{ shownHint }}</small>
  </div>

  <div v-else-if="control === 'slider'" class="gs-slide" :class="{ 'is-off': off }">
    <span class="gs-slide__head"><span class="gs-lab-row"><span class="gs-lab">{{ name }}</span><GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></span><span class="gs-slide__val">{{ readout }}</span></span>
    <MSlider :model-value="Number(value)" size="m" :label="name" :min="field.min" :max="field.max" :step="field.step" :decimals="field.decimals" :log="field.log" :unit="field.unit" :marks="field.marks" :disabled="off" @update:model-value="set" />
    <small v-if="shownHint" class="gs-hint">{{ shownHint }}</small>
  </div>

  <div v-else-if="control === 'number'" class="gs-num" :class="{ 'is-off': off }">
    <MField :model-value="numberText" type="number" :label="name" :suffix="(field.unit || '').trim()" :min="field.min" :max="field.max" :step="field.step || 1" :disabled="off" :error="numberError" @update:model-value="setNumber" />
    <span class="gs-mark-row"><GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></span>
  </div>

  <div v-else-if="control === 'args'" class="gs-args" :class="{ 'is-off': off }">
    <MField :model-value="String(value ?? '')" type="textarea" :rows="3" :label="name" :placeholder="words.argsPlace" :disabled="off" :error="argsError" :supporting="words.argsHint" @update:model-value="set" />
    <span class="gs-mark-row"><GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></span>
  </div>

  <div v-else-if="control === 'text'" class="gs-num" :class="{ 'is-off': off }">
    <MField :model-value="String(value ?? '')" :label="name" :disabled="off" @update:model-value="set" />
    <span class="gs-mark-row"><GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></span>
  </div>

  <GameSelect v-else :model-value="shownValue" :label="name" :options="choices" :disabled="off" :hint="shownHint" :search-label="words.search" :empty-text="words.searchNone" :close-label="words.closeSheet" @update:model-value="set">
    <template #mark><GameMark :held="held" :words="words" :name="name" :disabled="disabled" @reset="back" /></template>
  </GameSelect>
</template>

<script setup>
import { computed } from 'vue'
import { MField, MIcon, MSeg, MSlider, MSwitch } from 'foundation-ui/vue'
import GameMark from './GameMark.vue'
import GameSelect from './GameSelect.vue'
import { GRID_CELL, customArgsProblem, outOfRange, segmentsNeed } from '../../utils/sparxie/gameFields.js'

// One cell of the game settings grid: the control of one described item. `enabledBy`: off while another toggle is off. `locks[key]`: taken over by something else, shown off with that text.
// What the game has now is said only where it differs from the value of the preset (a small line, or 「当前」 on the option in the menu).
const props = defineProps({
  field: { type: Object, required: true },
  control: { type: String, required: true },
  values: { type: Object, required: true },
  defaults: { type: Function, required: true },
  names: { type: Object, default: () => ({}) },
  optionName: { type: Function, default: null },
  current: { type: Object, default: null },
  words: { type: Object, required: true },
  disabled: { type: Boolean, default: false },
  locks: { type: Object, default: () => ({}) },
  zh: { type: Boolean, default: true },
})
const f = computed(() => props.field)
// An item the person did not set is not written at launch: the game keeps what it has, so that is what the item shows (what the host read just now), and only without a reading the item's default.
// A value the item cannot show (a reading outside its options) is not used.
const ownValue = (k) => {
  const v = props.current && props.current[k] !== undefined && props.current[k] !== null ? props.current[k] : undefined
  if (v === undefined) return undefined
  const fld = k === props.field.key ? props.field : null
  if (fld && fld.kind === 'choice' && Array.isArray(fld.options) && fld.options.length && typeof fld.normalize !== 'function' && !fld.options.some((o) => o.value === v)) return undefined
  return v
}
const effective = (k) => props.values[k] ?? ownValue(k) ?? props.defaults(k, props.field)
const value = computed(() => effective(props.field.key))
const shownValue = computed(() => (typeof props.field.normalize === 'function' ? props.field.normalize(value.value) : value.value))
const set = (v) => { props.values[props.field.key] = v }
// an item the application holds is one the person changed; every other item follows the game and is not stored
const held = computed(() => props.values[props.field.key] !== undefined)
const back = () => { delete props.values[props.field.key] }
const flip = (event) => { if (props.control === 'switch' && !props.disabled && !(event.target.closest && event.target.closest('.m-switch')) && !off.value) set(!value.value) }
const lockOf = computed(() => props.locks[props.field.key] || '')
const off = computed(() => props.disabled || (!!props.field.enabledBy && !effective(props.field.enabledBy)))
const name = computed(() => props.names[props.field.key] || (props.zh ? f.value.label || f.value.labelEn : f.value.labelEn || f.value.label) || props.field.key)
const labelOf = (o) => (props.zh ? o.label || o.labelEn : o.labelEn || o.label) || (props.optionName ? props.optionName(props.field, o.value) : '') || String(o.value)
const currentValue = computed(() => {
  const k = props.field.key
  const v = props.current && props.current[k] !== undefined && props.current[k] !== null ? props.current[k] : undefined
  return v !== undefined && typeof props.field.normalize === 'function' ? props.field.normalize(v) : v
})
// the game's own value is mentioned where it is not the one the preset holds
const differs = computed(() => currentValue.value !== undefined && currentValue.value !== shownValue.value && !(props.control === 'slider' && Math.abs(Number(currentValue.value) - Number(value.value)) < 1e-9))
const options = computed(() => (props.field.options || []).map((o) => ({ value: o.value, label: labelOf(o) })))
// segments that do not fit one cell of the grid (three names of DirectX 11 length, four languages) take the whole row instead of being cut
const wide = computed(() => props.control === 'segments' && segmentsNeed(options.value.map((o) => o.label)) > GRID_CELL)
const choices = computed(() => options.value.map((o) => ({ ...o, text: currentValue.value === o.value ? props.words.current : '' })))
// the host's own note of the item (Star Rail's 120 unlock) sits right under its control; so does what the page knows about an item (the priority switch)
const noteOf = computed(() => (props.zh ? props.field.note : props.field.noteEn || props.field.note) || props.words.hints?.[props.field.key] || '')
const currentText = computed(() => {
  if (!differs.value || !props.words.current) return ''
  const v = currentValue.value
  if (props.control === 'switch') return `${props.words.current} ${v ? props.words.on : props.words.off}`
  if (props.control === 'select') return '' // the menu marks it
  const o = props.control === 'segments' ? options.value.find((x) => x.value === v) : null
  const shown = props.control === 'slider' && typeof v === 'number' ? v.toFixed(props.field.decimals ?? 0) : String(v)
  return `${props.words.current} ${o ? o.label : shown}${props.control === 'slider' ? props.field.unit || '' : ''}`
})
// one small line: the note of the item, or what the game has now
const shownHint = computed(() => noteOf.value || currentText.value)
const readout = computed(() => `${Number(value.value).toFixed(props.field.decimals ?? 0)}${props.field.unit || ''}`)

// a typed number: a text that is not a number (or empty) is held in the field and not stored; a number outside the range is told so and not stored either
const numberText = computed(() => (value.value === undefined || value.value === null ? '' : value.value))
const numberError = computed(() => (outOfRange(props.field, value.value) ? props.words.outOfRange(props.field.min, props.field.max) : ''))
function setNumber(v) {
  const n = Number(v)
  if (v === '' || !Number.isFinite(n)) return
  set(Math.round(n))
}

const problem = computed(() => (props.control === 'args' ? customArgsProblem(value.value) : null))
const argsError = computed(() => {
  const p = problem.value
  if (!p) return ''
  return p.kind === 'quote' ? props.words.argsQuote(p.line) : props.words.argsChar(p.line, p.ch)
})
</script>
