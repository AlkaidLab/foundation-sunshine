<template>
  <template v-for="f in fields" :key="f.key">
    <!-- an item for some inputs only folds away for the others (its height eases); the other items are plain rows of the group -->
    <div v-if="f.inputs && perRow" class="spx-row" :class="{ 'spx-row--open': shown(f) }" :inert="!shown(f) || undefined">
      <div class="spx-row__in"><SparxieFieldRow :field="f" v-bind="shared" /></div>
    </div>
    <SparxieFieldRow v-else :field="f" v-bind="shared" />
  </template>
</template>

<script setup>
import { computed } from 'vue'
import { forInput } from '../../utils/sparxie/model.js'
import SparxieFieldRow from './SparxieFieldRow.vue'

// One row per described item of a title (the host sends what exists: kind, options, range). The rows follow the host's marks, never a choice made here:
//   `enabledBy`  the item is off while another toggle is off (the target frame rate while the unlock is off)
//   `inputs`     the inputs an item is for: an item for touch only is hidden for keyboard, mouse and gamepad and opens again with touch (the page folds a whole group of such items itself: perRow = false)
//   `locks`      { key: text }: an item that something else has taken over (the game's frame rate while Sparxie unlocks it) is shown off with that text
const props = defineProps({
  fields: { type: Array, required: true },
  /** the object the values live in (changed in place) */
  values: { type: Object, required: true },
  /** (key, field) -> the value of an item that has none yet */
  defaults: { type: Function, required: true },
  /** key -> the name shown (the words of the page; a field the page has no words for shows the host's own label) */
  names: { type: Object, default: () => ({}) },
  /** (field, value) -> the words of an option */
  optionName: { type: Function, default: null },
  /** the values the game has now: shown small beside the item that holds ours */
  current: { type: Object, default: null },
  /** { current, on, off }: the words of the small current-value line */
  words: { type: Object, default: () => ({ current: '', on: '', off: '' }) },
  disabled: { type: Boolean, default: false },
  locks: { type: Object, default: () => ({}) },
  notes: { type: Object, default: () => ({}) },
  /** the input the application is for; the items for other inputs are hidden */
  input: { type: String, default: '' },
  perRow: { type: Boolean, default: true },
  cancelLabel: { type: String, default: 'Cancel' },
  zh: { type: Boolean, default: true },
})
const shown = (f) => !props.input || forInput(f, props.input)
const shared = computed(() => ({ values: props.values, defaults: props.defaults, names: props.names, optionName: props.optionName, current: props.current, words: props.words, disabled: props.disabled, locks: props.locks, notes: props.notes, cancelLabel: props.cancelLabel, zh: props.zh }))
</script>

<style scoped>
/* a row that is not for the chosen input folds to nothing: the grid row eases between 0fr and 1fr */
.spx-row { display: grid; grid-template-rows: 0fr; opacity: 0; transition: grid-template-rows var(--m-motion-medium, 240ms) cubic-bezier(0.2, 0, 0, 1), opacity 160ms linear; }
.spx-row--open { grid-template-rows: 1fr; opacity: 1; }
.spx-row__in { min-height: 0; overflow: hidden; }
.spx-row--open .spx-row__in { overflow: visible; }
.spx-row :deep(.m-item) { border-radius: 0; }
@media (prefers-reduced-motion: reduce) { .spx-row { transition: none; } }
</style>
