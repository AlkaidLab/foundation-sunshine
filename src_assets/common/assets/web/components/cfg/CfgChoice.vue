<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MChoiceItem, MInfo, MItem, MSeg } from 'foundation-ui/vue'
import { useCfgPin } from '../../composables/useCfgPin.js'
import { useOptionLabels } from '../../composables/optionLabels.js'
import { segFits, useRowWidth } from '../../composables/useSegFit.js'

// One configuration setting with a fixed set of values. Few choices with short names are a connected button group (the
// whole choice in view); a long list is the value row that opens a radio dialog.
// options: [{ value, label }] (labels already translated). The configuration keeps the value as the option gives it.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  options: { type: Array, required: true },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  icon: { type: String, default: '' },
  /** use the platform variant of the locale keys */
  tp: { type: Boolean, default: false },
  disabled: { type: Boolean, default: false },
  /** option assumed when the key is unset */
  def: { type: [String, Number, Boolean], default: undefined },
  /** { value: icon name } shown inside the buttons of the group */
  icons: { type: Object, default: () => ({}) },
  /** force the dialog row even for a short list */
  picker: { type: Boolean, default: false },
})
const { title, info } = useCfgText(props)
useOptionLabels(props)
const pin = useCfgPin(props, title)
const settings = () => props.config
// compare as text: a number kept in the file and "2" from a select mean the same option
const model = computed({
  get: () => props.options.find((o) => String(o.value) === String(settings()[props.k] ?? props.def))?.value ?? '',
  set: (value) => { settings()[props.k] = value },
})
// the name without its explanation in brackets: "挂起（S3 睡眠）" is "挂起" on its button
const short = (label) => String(label).split(/\s*[（(]/)[0]
const width = (text) => [...text].reduce((sum, ch) => sum + (ch.charCodeAt(0) > 0x2e80 ? 2 : 1), 0)
// the row's own width decides too: names that would be cut off are the value row that opens the list (the icon of the row indents the control by 40 px)
const rowWidth = useRowWidth(() => 32 + (props.icon ? 40 : 0))
const asGroup = computed(() => !props.picker && props.options.length >= 2 && props.options.length <= 4 && props.options.every((o) => width(short(o.label)) <= 18) && segFits(props.options.map((o) => short(o.label)), rowWidth.value))
const groupOptions = computed(() => props.options.map((o) => ({ value: o.value, label: short(o.label), ...(props.icons[o.value] ? { icon: props.icons[o.value] } : {}) })))
</script>

<template>
  <MItem v-if="asGroup" :data-cfg="k" :title="title" :icon="icon" :disabled="disabled" :pin="pin" stack>
    <template v-if="info" #trail><MInfo :text="info" /></template>
    <template #control><MSeg v-model="model" block :label="title" :options="groupOptions" :disabled="disabled" /></template>
  </MItem>
  <MChoiceItem v-else v-model="model" :data-cfg="k" :pin="pin" :title="title" :options="options" :icon="icon" :help="info" :disabled="disabled" :cancel-label="$t('_common.cancel')" :search-label="$t('_common.search')" :empty-text="$t('_common.no_results')" :close-label="$t('_common.close')" />
</template>
