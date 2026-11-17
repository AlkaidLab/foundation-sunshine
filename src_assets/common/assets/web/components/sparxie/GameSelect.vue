<template>
  <div class="gs-sel" :class="{ 'is-open': open, 'is-off': disabled }">
    <span class="gs-lab-row"><span :id="labelId" class="gs-lab">{{ label }}</span><slot name="mark" /></span>
    <button :id="buttonId" type="button" class="gs-sel__btn" :aria-labelledby="`${labelId} ${buttonId}`" :aria-haspopup="sheet ? 'dialog' : 'menu'" :aria-expanded="open" :disabled="disabled" @click="toggle" @keydown="onButtonKey">
      <span class="gs-sel__val">{{ shown }}</span>
      <MIcon class="gs-sel__arrow" name="keyboard_arrow_down" />
    </button>
    <small v-if="hint" class="gs-hint">{{ hint }}</small>

    <!-- the menu is made when the select is first opened: a page of forty selects draws forty buttons, not six hundred menu items -->
    <template v-if="made">
      <MPickSheet v-if="sheet" v-model="model" v-model:open="open" :title="label" :options="options" :search-label="searchLabel" :empty-text="emptyText" :close-label="closeLabel" :search-from="7" />
      <md-menu v-else ref="menu" class="gs-menu" :anchor="buttonId" positioning="popover" @close="open = false" @keydown="onMenuKey">
        <md-menu-item v-for="o in options" :key="String(o.value)" :data-value="String(o.value)" :class="{ 'is-on': o.value === model }" @click="onClick(o)">
          <span slot="start" class="gs-sel__tick"><MIcon v-if="o.value === model" name="check" /></span>
          <div slot="headline">{{ o.label }}</div>
          <span v-if="o.text" slot="end" class="gs-sel__now">{{ o.text }}</span>
        </md-menu-item>
      </md-menu>
    </template>
  </div>
</template>

<script setup>
import { computed, nextTick, ref, useId } from 'vue'
import { MIcon, MPickSheet } from 'foundation-ui/vue'

// A compact select of the game settings grid: the name above, the value in a button, and the choices in a Material menu (a list of more than seven opens a sheet with a search field).
// The option the game has right now carries `text` (「当前」); `hint` is one small line under the control.
const model = defineModel({ type: [String, Number, Boolean], default: '' })
const props = defineProps({
  label: { type: String, required: true },
  /** [{ value, label, text? }] */
  options: { type: Array, required: true },
  disabled: { type: Boolean, default: false },
  hint: { type: String, default: '' },
  searchLabel: { type: String, default: 'Search' },
  emptyText: { type: String, default: 'No matches' },
  closeLabel: { type: String, default: 'Close' },
})
const id = useId()
const labelId = `${id}-l`
const buttonId = `${id}-b`
const open = ref(false)
const made = ref(false)
const menu = ref(null)
const sheet = computed(() => props.options.length > 7)
const shown = computed(() => props.options.find((o) => o.value === model.value)?.label ?? '')

async function toggle() {
  if (props.disabled) return
  if (!made.value) { made.value = true; await nextTick() }
  if (sheet.value) { open.value = true; return }
  const el = menu.value
  if (!el) return
  await customElements.whenDefined('md-menu')
  // the menu is as wide as the button, at least
  el.style.minWidth = `${document.getElementById(buttonId)?.offsetWidth || 0}px`
  el.open = !el.open
  open.value = el.open
}
function pick(o) { model.value = o.value; open.value = false }
// Enter and Space pick the row that has the focus (the Material menu moves with the keys; the pick itself goes through the click of the row, which a key does not always send), once
let keyPickAt = 0
function onMenuKey(event) {
  if (event.key !== 'Enter' && event.key !== ' ') return
  const row = event.target.closest?.('md-menu-item')
  const o = row && props.options.find((x) => String(x.value) === row.dataset.value)
  if (!o) return
  keyPickAt = Date.now(); event.preventDefault(); pick(o)
  menu.value.open = false
}
const onButtonKey = (event) => { if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); toggle() } }
// a click right after the key is the same pick
const onClick = (o) => { if (Date.now() - keyPickAt > 120) pick(o) }
</script>
