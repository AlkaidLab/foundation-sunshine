<template>
  <div class="spx-sections">
    <template v-for="g in groups" :key="g.section">
      <!-- a group that is only for some inputs (the touch interface) folds away as a whole when the input is another one -->
      <div class="spx-fold" :class="{ 'spx-fold--open': g.open }" :inert="!g.open || undefined">
        <div class="spx-fold__in">
          <MSection :title="titles[g.section] || ''">
            <template v-if="g.section !== 'input'" #action><MChip tone="tertiary" :image="SPARXIE_MARK">Sparxie</MChip></template>
            <MGroup>
              <SparxieFieldRows :fields="g.fields" :values="o" :defaults="defaults" :names="names" :option-name="optionName" :words="words" :input="input" :notes="notes" :per-row="!g.folds" :cancel-label="C.cancel" :zh="zh" />
            </MGroup>
            <MFooter v-if="g.section === 'input'">{{ C.modeTouchText }}</MFooter>
          </MSection>
        </div>
      </div>
    </template>

    <MSection :title="C.secProfile">
      <MGroup>
        <MSwitchItem v-model="o.isDefault" icon="star" :title="C.makeDefault" />
        <MItem v-if="b.profile" :title="C.profileId"><template #default><span class="spx-mono">{{ b.profile }}</span></template></MItem>
      </MGroup>
      <MFooter>{{ C.syncNote }}</MFooter>
    </MSection>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { MChip, MFooter, MGroup, MItem, MSection, MSwitchItem } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { SPARXIE_DEFAULTS, SPARXIE_MARK, forInput } from '../../utils/sparxie/model.js'
import SparxieFieldRows from './SparxieFieldRows.vue'

// Sparxie's own settings of a game app that launches through it (every app of a Sparxie title does while the component is installed). The items are the host's list for the title
// (frame-rate unlock and its target tiers, the in-game preset mapping, the background limit, the process priority, the touch interface scale); the page draws what the host describes.
// The input (touch, or keyboard, mouse and gamepad) shows or hides nothing here except the items that are for touch only, which the host marks. Written to Sparxie's config when the app is saved.
const props = defineProps({
  /** { game, profile, options }: changed in place */
  block: { type: Object, required: true },
  /** the items of the title (model.sparxieFieldsOf) */
  fields: { type: Array, required: true },
})
const C = useSparxieCopy()
const { locale } = useI18n()
const zh = computed(() => locale.value.toLowerCase().startsWith('zh'))
const b = props.block
const o = b.options
const input = computed(() => o.input || SPARXIE_DEFAULTS.input)
const defaults = (key, field) => (field && field.def !== undefined ? field.def : SPARXIE_DEFAULTS[key])
const words = computed(() => ({ current: '', on: '', off: '' }))

// a stored value the host's list has no tier for (a free number written by an older build) stays a choice of its own
const withStored = (f) => {
  if (f.kind !== 'choice') return f
  const stored = o[f.key]
  return stored === undefined || f.options.some((x) => x.value === stored) ? f : { ...f, options: [...f.options, { value: stored, label: '', labelEn: '' }] }
}
const ORDER = ['input', 'fps', 'advanced', 'ui']
const groups = computed(() => {
  const by = new Map()
  // the size of an Endfield session is the app's 显示 choice (跟随串流 or 固定), drawn by its own section
  for (const f of props.fields) { if (f.key === 'resolution') continue; const s = f.section || 'advanced'; if (!by.has(s)) by.set(s, []); by.get(s).push(withStored(f)) }
  return [...by.entries()].sort((a, c) => (ORDER.indexOf(a[0]) + 1 || 99) - (ORDER.indexOf(c[0]) + 1 || 99)).map(([section, fields]) => {
    const folds = fields.every((f) => f.inputs)
    return { section, fields, folds, open: !folds || fields.some((f) => forInput(f, input.value)) }
  })
})
const titles = computed(() => ({ input: C.value.secSparxie, fps: C.value.secFps, advanced: C.value.secAdv, ui: C.value.secUi }))
const names = computed(() => ({ input: C.value.inputName, resolution: C.value.resolution, fpsUnlock: C.value.fpsUnlock, fps: C.value.fpsCap, bgLimit: C.value.bgLimit, bgFps: C.value.bgFps, priority: C.value.priority, followPreset: C.value.followPreset, preset30: C.value.p30, preset60: C.value.p60, scaleOverride: C.value.scaleOverride, scale: C.value.scale }))
const notes = computed(() => ({}))
const PRIORITIES = ['belowNormal', 'normal', 'aboveNormal', 'high']
const optionName = (f, v) => {
  if (f.key === 'input') return v === 'kbm' ? C.value.inputKbm : C.value.inputTouch
  if (f.key === 'priority') return C.value.priorityOpts[PRIORITIES.indexOf(v)] || String(v)
  if (f.key === 'resolution') return v === 'auto' ? C.value.resolutionAuto : String(v)
  if (v === 'display') return C.value.fpsDisplay
  return typeof v === 'number' ? `${v} FPS` : String(v)
}
</script>

<style scoped>
.spx-sections { display: contents; }
.spx-mono { font-family: var(--m-font-mono, ui-monospace, monospace); font-size: 12.5px; color: var(--md-sys-color-on-surface-variant); overflow-wrap: anywhere; }
/* a whole group for one input folds to nothing for the others: the height eases and the page gap goes with it */
.spx-fold { display: grid; grid-template-rows: 0fr; opacity: 0; margin-top: -4px; transition: grid-template-rows var(--m-motion-medium, 240ms) cubic-bezier(0.2, 0, 0, 1), margin-top var(--m-motion-medium, 240ms) cubic-bezier(0.2, 0, 0, 1), opacity 160ms linear; }
.spx-fold--open { grid-template-rows: 1fr; opacity: 1; margin-top: 0; }
.spx-fold__in { min-height: 0; overflow: hidden; }
.spx-fold--open .spx-fold__in { overflow: visible; }
@media (prefers-reduced-motion: reduce) { .spx-fold { transition: none; } }
</style>
