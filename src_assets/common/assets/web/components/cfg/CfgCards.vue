<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MChoiceCards } from 'foundation-ui/vue'

// One configuration setting with a few values as large cards that show what each does (the `art` slot draws it).
// Tier 1: the control that leads its page. The value is compared as text like CfgChoice.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  options: { type: Array, required: true },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  tp: { type: Boolean, default: false },
  def: { type: [String, Number, Boolean], default: undefined },
  icons: { type: Object, default: () => ({}) },
  columns: { type: Number, default: 0 },
  disabled: { type: Boolean, default: false },
  /** no line of help under the cards */
  quiet: { type: Boolean, default: false },
})
const { title, info } = useCfgText(props)
const model = computed({
  get: () => props.options.find((o) => String(o.value) === String(props.config[props.k] ?? props.def))?.value ?? '',
  set: (value) => { props.config[props.k] = value },
})
const cards = computed(() => props.options.map((o) => ({ ...o, ...(props.icons[o.value] ? { icon: props.icons[o.value] } : {}) })))
</script>

<template>
  <div class="cfg-cards" :data-cfg="k">
    <MChoiceCards v-model="model" :options="cards" :label="title" :columns="columns" :disabled="disabled">
      <template #art="slot"><slot name="art" v-bind="slot" /></template>
    </MChoiceCards>
    <p v-if="info && !quiet" class="cfg-cards__help">{{ info }}</p>
  </div>
</template>
