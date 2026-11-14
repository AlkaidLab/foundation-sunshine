<script setup>
import { computed } from 'vue'
import { useCfgText } from '../../composables/useCfgText.js'
import { MChoiceItem, MInfo, MItem, MSeg } from 'foundation-ui/vue'
import { segFits, useRowWidth } from '../../composables/useSegFit.js'

// One setting with a small ordered set of named values (an encoder preset), in place of a slider over steps. Up to seven short
// values are connected segments (the chosen one filled and morphed), with an axis under them when the two ends mean something
// (`axisStart`, `axisEnd`); a longer list, or longer names, is the searchable sheet picker. The value is stored as the option
// gives it. No number on the axis is invented: the axis only says which way the choice goes.
const props = defineProps({
  config: { type: Object, required: true },
  k: { type: String, required: true },
  /** [{ value, label }] */
  options: { type: Array, required: true },
  def: { type: [String, Number], default: undefined },
  label: { type: String, default: '' },
  help: { type: String, default: '' },
  tp: { type: Boolean, default: false },
  icon: { type: String, default: '' },
  axisStart: { type: String, default: '' },
  axisEnd: { type: String, default: '' },
  disabled: { type: Boolean, default: false },
})
const { t, title, info } = useCfgText(props)
const model = computed({
  get: () => props.options.find((o) => String(o.value) === String(props.config[props.k] ?? props.def))?.value ?? '',
  set: (value) => { props.config[props.k] = value },
})
// a short name on a button of the narrowest width that is still a finger (44 px, 2 px between: the 7 presets of an encoder fit a phone of 360 px)
// (on a phone the group takes the whole width of the row, the icon's indent and 10 px of the row's padding either side included: see the style below)
const rowWidth = useRowWidth(() => (window.innerWidth < 600 ? 12 : 72))
const segmented = computed(() => props.options.length >= 2 && props.options.length <= 7 && props.options.every((o) => [...String(o.label)].length <= 4) && segFits(props.options.map((o) => o.label), rowWidth.value, { pad: 4, gap: 2 }))
</script>

<template>
  <MItem v-if="segmented" :data-cfg="k" :title="title" :icon="icon" stack :disabled="disabled">
    <template v-if="info" #trail><MInfo :text="info" /></template>
    <template #control>
      <div class="cfg-steps">
        <MSeg v-model="model" block :label="title" :options="options" :disabled="disabled" />
        <div v-if="axisStart || axisEnd" class="cfg-steps__axis" aria-hidden="true"><span>{{ axisStart }}</span><i /><span>{{ axisEnd }}</span></div>
      </div>
    </template>
  </MItem>
  <MChoiceItem v-else v-model="model" :data-cfg="k" :title="title" :icon="icon" :options="options" :help="info" :disabled="disabled" :sheet-from="0" :search-label="t('_common.search')" :empty-text="t('_common.no_results')" :close-label="t('_common.close')" :cancel-label="t('_common.cancel')" />
</template>

<style>
.cfg-steps { display: flex; flex-direction: column; gap: 6px; }
/* a phone: the presets use the whole width of the row, there is no room for an indent */
@media (max-width: 599.98px) {
  .m-item--stack .m-item__control:has(> .cfg-steps) { padding-left: 0; }
  .cfg-steps { margin-inline: -10px; }
  .cfg-steps .m-seg { gap: 2px; flex-wrap: nowrap; }
  .cfg-steps .m-seg .m-seg__btn { min-width: 0; padding-inline: 0; }
  .cfg-steps__axis { padding-inline: 14px; }
}
.cfg-steps__axis { display: flex; align-items: center; gap: 8px; padding: 0 4px; font: var(--m-label-small, 500 11px/16px sans-serif); color: var(--md-sys-color-on-surface-variant); }
.cfg-steps__axis i { flex: 1; height: 2px; border-radius: 1px; background: linear-gradient(90deg, var(--md-sys-color-outline-variant), var(--md-sys-color-primary)); opacity: .7; }
</style>
