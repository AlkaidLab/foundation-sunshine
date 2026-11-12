<template>
  <MSection v-if="row && row.settingsManaged === false" :title="C.secGameSettings">
    <!-- a title the host manages no game settings of: said once, and it is not a limit of the game version -->
    <MBanner tone="info" icon="info">{{ C.gsNotManaged(gameName) }}</MBanner>
  </MSection>
  <MSection v-else :title="C.secGameSettings">
      <MBanner v-if="running" tone="info" icon="lock">{{ C.gsRunning }}</MBanner>
      <MBanner v-if="unsupportedText" tone="info" icon="info">{{ unsupportedText }}</MBanner>

      <!-- one card per group of the game's own settings menu: menus and segments in a grid that goes from three columns to one with the width of the page, then sliders, switches and typed values -->
      <section v-for="g in groups" :key="g.section" class="gs-card" :aria-labelledby="`gs-${g.section}`">
        <h3 :id="`gs-${g.section}`" class="gs-card__title">{{ g.title }}</h3>
        <template v-for="(run, i) in g.runs" :key="i">
          <div class="gs-grid" :class="[`gs-grid--${run.family}`, { 'gs-grid--one': run.cells.length === 1 }]">
            <GameField v-for="cell in run.cells" :key="cell.field.key" :class="{ 'gs-args-cell': cell.control === 'args', 'gs-noted': !!cell.field.note }" :field="cell.field" :control="cell.control" :values="block.settings" :defaults="defaults" :names="names" :option-name="optionName" :current="current" :words="words" :disabled="false" :locks="locks" :zh="zh" />
          </div>
        </template>
        <MFooter v-if="fpsHint && g.hasFps">{{ fpsHint }}</MFooter>
      </section>

      <MGroup>
        <MItem v-if="heldKeys.length" stack>
          <template #title>{{ C.restoreAfter }}</template>
          <template #control><MSeg v-model="block.restore" block :label="C.restoreAfter" :options="Object.entries(C.restoreOpts).map(([value, label]) => ({ value, label }))" /></template>
        </MItem>
        <MItem action icon="undo" :title="C.gsAllFollow" :disabled="!heldKeys.length" @click="followAll" />
      </MGroup>
      <MFooter v-if="heldKeys.length && block.restore !== 'keep'">{{ C.restoreNote }}</MFooter>
  </MSection>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { MBanner, MFooter, MGroup, MItem, MSection, MSeg } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { gameDefaults, gameFieldsOf, splitUnavailable } from '../../utils/sparxie/model.js'
import { SECTION_TITLES, firstProblem, isHidden, layoutGroups } from '../../utils/sparxie/gameFields.js'
import GameField from './GameField.vue'
import './gameSettings.css'

// The game's own settings of one game app: the items the game's own settings menu has (the host describes them per title: resolution, display mode, frame-rate tier, graphics, sound, language, launch), laid out like
// the page of the community launchers (a grid of compact menus, sliders for volumes, searchable menus for languages, the launch arguments as a field of their own) in the language of this product.
// An application holds only the items the person changed (sparse overrides). Every other item follows the game: it shows the game's current value, marked 跟随游戏, and stays editable;
// editing it makes it this application's own value (marked 已更改, with a way back, and 全部跟随游戏 for all of them). The host writes only the held items at launch and puts them back when the session ends.
// Nothing is held when the page opens, so opening never changes anything. While the game runs the host refuses a write.
// The game's frame rate is taken over while Sparxie unlocks the frame rate (`fpsBySparxie`): the item is off, says so, and the value is not written.
const props = defineProps({
  /** { game, settings, restore }: changed in place; settings holds only the items the person changed */
  block: { type: Object, required: true },
  /** the catalog row of the title: the host's description of its items */
  row: { type: Object, default: null },
  /** what the host reads the game's current values from: the install id, or the executable and region of an install the host does not store yet */
  installId: { type: String, default: '' },
  exe: { type: String, default: '' },
  region: { type: String, default: '' },
  /** Sparxie's frame-rate unlock is on for this application: the game's own frame rate is Sparxie's */
  fpsBySparxie: { type: Boolean, default: false },
  /** this application has Sparxie's target frame rate (it starts through Sparxie and the title has it) */
  sparxieFps: { type: Boolean, default: false },
  /** the host's answer to the first read ({ values, running, unavailable, screen }) when the page already has it; the page asks again while the preset is in use */
  initial: { type: Object, default: null },
})
// the first value of the preset that the host would refuse, as a line for the save button ('' when there is none)
const emit = defineEmits(['problem'])
const C = useSparxieCopy()
const { client } = useSparxie()
const { locale } = useI18n()
const zh = computed(() => locale.value.toLowerCase().startsWith('zh'))
const current = ref(props.initial?.values || {})
const running = ref(!!props.initial?.running)
const unavailable = ref(props.initial?.unavailable || [])
const gameName = computed(() => (zh.value ? props.row?.name || props.row?.nameEn : props.row?.nameEn || props.row?.name) || props.block.game)
const screen = ref(props.initial?.screen || null)

const split = computed(() => splitUnavailable(gameFieldsOf(props.row, props.block.game, Object.keys(C.value.presetOpts)).filter((f) => !isHidden(props.block.game, f.key)), unavailable.value))
const fields = computed(() => split.value.shown)
const unsupportedText = computed(() => (split.value.hidden.length ? C.value.gsUnsupported(split.value.hidden.map((f) => names.value[f.key] || (zh.value ? f.label || f.labelEn : f.labelEn || f.label) || f.key).join(C.value.listSep)) : ''))
// the game's own frame-rate tiers end below what people look for (Genshin Impact: 60): say where the higher rates are
const fpsHint = computed(() => {
  const tiers = (fields.value.find((f) => f.key === 'fps')?.options || []).map((o) => o.value).filter((v) => typeof v === 'number' && v > 0)
  const top = tiers.length ? Math.max(...tiers) : 0
  return props.sparxieFps && !props.fpsBySparxie && top > 0 && top < 120 ? C.value.gsFpsHint(top) : ''
})
const names = computed(() => ({ resolution: C.value.resolution, display: C.value.displayMode, fps: C.value.frameRate, preset: C.value.preset, renderScale: C.value.renderScale, vsync: C.value.vsync, hdr: C.value.hdr, resolutionIndex: C.value.gameResolution }))
const nameOf = (f) => names.value[f.key] || (zh.value ? f.label || f.labelEn : f.labelEn || f.label) || f.key

// the controls of a run are of one family: menus and segments, sliders, switches, typed values; each family has the grid that suits its width
const FAMILY = { select: 'pick', segments: 'pick', slider: 'slide', switch: 'tog', number: 'type', text: 'type', args: 'type' }
const layout = computed(() => layoutGroups(fields.value, { game: props.block.game, zh: zh.value, screen: screen.value, C: C.value, labelOf: (f, o) => (zh.value ? o.label || o.labelEn : o.labelEn || o.label) || optionName(f, o.value) }))
const groups = computed(() => layout.value.map((g) => {
  const runs = []
  for (const cell of g.cells) {
    const family = FAMILY[cell.control] || 'type'
    const last = runs[runs.length - 1]
    if (last && last.family === family) last.cells.push(cell)
    else runs.push({ family, cells: [cell] })
  }
  return { section: g.section, hasFps: g.cells.some((c) => c.field.key === 'fps'), title: C.value[SECTION_TITLES[g.section]] || '', runs }
}))
const words = computed(() => ({ follow: C.value.gsFollow, changed: C.value.gsChanged, back: C.value.gsBack, current: C.value.gsNow, on: C.value.on, off: C.value.off, search: C.value.search, searchNone: C.value.searchNone, closeSheet: C.value.closeSheet, outOfRange: C.value.outOfRange, argsHint: C.value.argsHint, argsPlace: C.value.argsPlace, argsChar: C.value.argsChar, argsQuote: C.value.argsQuote, hints: C.value.hints }))
const locks = computed(() => (props.fpsBySparxie ? { fps: C.value.fpsBySparxie } : {}))
const optionName = (f, v) => {
  if (f.key === 'display') return C.value.display[v] || String(v)
  if (f.key === 'fps') return C.value.fpsOpt(v)
  if (f.key === 'preset') return C.value.presetOpts[v] || String(v)
  return String(v)
}
const defaults = (key, f) => (gameDefaults(props.block.game)[key] !== undefined ? gameDefaults(props.block.game)[key] : f && f.def !== undefined ? f.def : f?.kind === 'toggle' ? false : f?.kind === 'slider' ? f.min : f?.options?.[0]?.value)

// what the host would refuse is said to the page that saves: the application is not saved with it
// an item that follows the game is not written, so only the held ones can be refused
const heldKeys = computed(() => Object.keys(props.block.settings).filter((k) => props.block.settings[k] !== undefined))
const problem = computed(() => {
  const heldLayout = layout.value.map((g) => ({ ...g, cells: g.cells.filter((c) => props.block.settings[c.field.key] !== undefined) }))
  const p = firstProblem(heldLayout, (key, f) => props.block.settings[key] ?? current.value[key] ?? defaults(key, f ?? fields.value.find((x) => x.key === key)))
  const f = p && fields.value.find((x) => x.key === p.key)
  return f ? C.value.settingsProblem(nameOf(f)) : ''
})
watch(problem, (text) => emit('problem', text), { immediate: true })
onBeforeUnmount(() => emit('problem', ''))

const followAll = () => { for (const k of Object.keys(props.block.settings)) delete props.block.settings[k] }

async function poll() {
  if (!props.installId && !props.exe) return
  try {
    const r = await client().gameSettings(props.installId, { exe: props.exe, region: props.region })
    current.value = r.values
    running.value = r.running
    unavailable.value = r.unavailable
    screen.value = r.screen || null
  } catch { /* what cannot be read is not shown */ }
}
let timer = 0
onMounted(() => { poll(); timer = setInterval(poll, 5000) })
onBeforeUnmount(() => clearInterval(timer))
watch(() => [props.installId, props.exe, props.region], poll)
</script>
