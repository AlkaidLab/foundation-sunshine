<template>
  <MSection v-if="shown" :title="C.secDisplay">
    <MGroup>
      <MItem stack>
        <template #control><MSeg :model-value="display.mode" block :label="C.secDisplay" :options="[{ value: 'follow', label: C.displayFollow }, { value: 'fixed', label: C.displayFixed }]" @update:model-value="setMode" /></template>
      </MItem>
    </MGroup>
    <MFooter>{{ line }}</MFooter>
    <section v-if="display.mode === 'fixed' && cells.length" class="gs-card" :aria-label="C.secDisplay">
      <div class="gs-grid gs-grid--pick" :class="{ 'gs-grid--one': cells.length === 1 }">
        <GameField v-for="cell in cells" :key="cell.field.key" :field="cell.field" :control="cell.control" :values="display" :defaults="defaults" :names="names" :words="words" :zh="zh" />
      </div>
    </section>
  </MSection>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { MFooter, MGroup, MItem, MSection, MSeg } from 'foundation-ui/vue'
import { useI18n } from 'vue-i18n'
import { useSparxie } from '../../composables/useSparxie.js'
import { useSparxieCopy } from '../../composables/useSparxieCopy.js'
import { controlOf } from '../../utils/sparxie/gameFields.js'
import { effectiveLine, hasDisplay, resolutionChoices, startFixed, windowChoices } from '../../utils/sparxie/display.js'
import GameField from './GameField.vue'
import './gameSettings.css'

// The 显示 choice of a game app. 跟随串流 (the default): the game runs fullscreen at the size of the display that is streamed, every launch; the one line under the choice says the size when a stream
// runs and the host can read it. 固定: the person's size and window mode. The choice is the app's own, whatever the settings mode; the host writes it into the game before it starts.
const props = defineProps({
  /** the app's sparxie block: { game, display }, display is changed in place */
  block: { type: Object, required: true },
  /** what the host reads the display from: the install id, or the executable and region of an install the host does not store yet */
  installId: { type: String, default: '' },
  exe: { type: String, default: '' },
  region: { type: String, default: '' },
})
const C = useSparxieCopy()
const { client } = useSparxie()
const { locale } = useI18n()
const zh = computed(() => locale.value.toLowerCase().startsWith('zh'))
const game = computed(() => props.block.game)
const shown = computed(() => hasDisplay(game.value))
const display = props.block.display
const screen = ref(null)
const stream = ref(null)

const names = computed(() => ({ resolution: C.value.resolution, window: C.value.displayMode, resolutionIndex: C.value.resolution }))
const words = computed(() => ({ current: '', on: '', off: '', search: C.value.search, searchNone: C.value.searchNone, closeSheet: C.value.closeSheet, outOfRange: C.value.outOfRange, argsHint: '', argsPlace: '', argsChar: C.value.argsChar, argsQuote: C.value.argsQuote, hints: C.value.hints }))
const defaults = (key) => (key === 'window' ? 'fullscreen' : key === 'resolutionIndex' ? '0' : undefined)
const line = computed(() => effectiveLine(display, game.value, { stream: stream.value, screen: screen.value }, C.value))

const cells = computed(() => {
  const out = []
  const choices = resolutionChoices(game.value, screen.value, display.resolution)
  if (game.value === 'zzz') {
    // a position in the game's own list, which it makes from the screen; the entry also says the window mode
    // without the display size a position means nothing: the window mode alone is offered then
    if (!choices.length) {
      const options = windowChoices('genshin').map((w) => ({ value: w, label: C.value.display[w], labelEn: C.value.display[w] }))
      const windowOnly = { key: 'window', kind: 'choice', options, def: 'fullscreen' }
      return [{ control: controlOf(windowOnly, zh.value), field: windowOnly }]
    }
    if (choices.length) out.push({ control: 'select', field: { key: 'resolutionIndex', kind: 'choice', options: choices.map((r) => ({ value: r.value, label: `${r.mode === 'windowed' ? C.value.display.windowed : C.value.display.fullscreen} · ${r.width}×${r.height}${r.native ? ` (${C.value.native})` : ''}`, labelEn: `${r.mode === 'windowed' ? C.value.display.windowed : C.value.display.fullscreen} · ${r.width}×${r.height}${r.native ? ` (${C.value.native})` : ''}` })), def: '0' } })
    return out
  }
  const sizes = { key: 'resolution', kind: 'choice', options: choices.map((r) => ({ value: r.value, label: r.value, labelEn: r.value })), def: choices[0]?.value }
  out.push({ control: controlOf(sizes, zh.value), field: sizes })
  const windows = windowChoices(game.value)
  if (windows.length) {
    const options = windows.map((w) => ({ value: w, label: C.value.display[w], labelEn: C.value.display[w] }))
    out.push({ control: controlOf({ kind: 'choice', options }, zh.value), field: { key: 'window', kind: 'choice', options, def: 'fullscreen' } })
  }
  return out
})

function setMode(mode) {
  if (mode === 'follow') {
    for (const k of Object.keys(display)) delete display[k]
    display.mode = 'follow'
    return
  }
  Object.assign(display, startFixed(game.value, screen.value))
}
// an entry of Zenless Zone Zero's list says its own window mode
watch(() => display.resolutionIndex, (index) => {
  if (game.value !== 'zzz' || display.mode !== 'fixed') return
  const entry = resolutionChoices('zzz', screen.value).find((r) => r.value === index)
  if (entry) display.window = entry.mode
})

async function poll() {
  if (!props.installId && !props.exe) return
  try {
    const r = await client().gameSettings(props.installId, { exe: props.exe, region: props.region })
    screen.value = r.screen || null
    stream.value = r.stream || null
  } catch { /* the line then says it follows the device */ }
}
let timer = 0
onMounted(() => { poll(); timer = setInterval(poll, 5000) })
onBeforeUnmount(() => clearInterval(timer))
watch(() => [props.installId, props.exe, props.region], poll)
</script>
