<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MDsPlot, MIcon, MLightbar } from 'foundation-ui/vue'
import TriggerPreview from './TriggerPreview.vue'
import { useDualsenseShared } from '../../composables/dualsenseShared.js'

// What the values on a card or a group look like, drawn from the values being edited (the store): the haptics signal
// after the gain, the rumble response of the two motors, the response of the sticks, the trigger axes and the gyro, the
// light bar and the player lights, the adaptive trigger preview. `id` is a card id (haptics, triggers, motion, lighting,
// audio) or a group id (g1 .. g12); `compact` is the small variant on the card of the gamepad page.
const props = defineProps({ id: { type: String, required: true }, compact: { type: Boolean, default: false } })
const { t } = useI18n()
const { edited, valueOf } = useDualsenseShared()

const num = (name, fallback) => Number(valueOf(name, fallback))
const str = (name, fallback) => String(valueOf(name, fallback))
const bool = (name, fallback = true) => valueOf(name, fallback) === true

const wave = computed(() => ({ gain: num('ds5.haptics.hd.gain', 1) }))
const rumble = computed(() => ({ enabled: bool('ds5.rumble.enabled'), strength: num('ds5.rumble.strength', 1), curve: num('ds5.rumble.curve', 1), gate: num('ds5.rumble.noiseGate', 0.02), low: num('ds5.rumble.lowGain', 1), high: num('ds5.rumble.highGain', 1) }))
const stick = (side) => ({ inner: num(`gamepad.stick.${side}.deadzoneInner`, 0), outer: num(`gamepad.stick.${side}.deadzoneOuter`, 1), anti: num(`gamepad.stick.${side}.antiDeadzone`, 0), sensitivity: num(`gamepad.stick.${side}.sensitivity`, 1), shape: str(`gamepad.stick.${side}.curve`, 'linear') })
const axis = (side) => ({ inner: num(`ds5.axis.trigger.${side}.rangeMin`, 0), outer: num(`ds5.axis.trigger.${side}.rangeMax`, 1), anti: num(`ds5.axis.trigger.${side}.antiDeadzone`, 0), shape: str(`ds5.axis.trigger.${side}.curve`, 'linear') })
const gyro = computed(() => ({ inner: num('gamepad.gyro.deadzone', 0.05), outer: 1, sensitivity: num('gamepad.gyro.sensitivity', 1) }))

// light bar: the colour the settings set, dark when off, a sweep when the game decides
const hex = (r, g, b) => '#' + [r, g, b].map((v) => Math.min(255, Math.max(0, Number(v) || 0)).toString(16).padStart(2, '0')).join('')
const bar = computed(() => {
  const mode = str('ds5.led.lightbar.mode', 'followGame')
  if (mode === 'off') return { mode: 'off', label: t('ds5.color.is_off') }
  if (mode !== 'fixed') return { mode: 'game', label: t('ds5.color.by_game') }
  const color = hex(valueOf('ds5.led.lightbar.colorR', 0), valueOf('ds5.led.lightbar.colorG', 0), valueOf('ds5.led.lightbar.colorB', 0))
  return { mode: 'fixed', color, label: color }
})
// the five player lights: the chosen player lights that many (auto: the first), off: none, followGame: the first
const players = computed(() => {
  const mode = str('ds5.led.player.mode', 'followGame')
  if (mode === 'off') return [false, false, false, false, false]
  const index = Number(valueOf('ds5.led.player.index', 0)) || 1
  return [0, 1, 2, 3, 4].map((i) => i < index)
})
const view = computed(() => {
  const id = props.id
  const g = /^g(\d+)$/.exec(id)?.[1]
  if (id === 'haptics') return ['wave', 'rumble']
  if (id === 'triggers' || g === '3') return ['trigger']
  if (id === 'motion' || g === '6') return ['gyro']
  if (id === 'lighting' || g === '8') return ['bar', 'players']
  if (id === 'audio' || g === '9') return ['route']
  if (g === '1') return ['wave']
  if (g === '2') return ['rumble']
  if (g === '4') return ['left', 'right']
  if (g === '5') return ['axisLeft', 'axisRight']
  return []
})
// the preview follows the value on the page (the row may still be disabled because this build does not apply it)
const route = computed(() => str('ds5.audio.route', 'auto'))
const triggerOn = computed(() => edited[24] !== false)
</script>

<template>
  <div v-if="view.length" class="ds5-live" :class="{ 'ds5-live--compact': compact }">
    <template v-for="kind in view" :key="kind">
      <div v-if="kind === 'wave'" class="ds5-live__plot"><MDsPlot kind="wave" :compact="compact" v-bind="wave" :label="t('ds5.plots.wave')" /><small v-if="!compact">{{ t('ds5.plots.wave') }}</small></div>
      <div v-else-if="kind === 'rumble'" class="ds5-live__plot"><MDsPlot kind="rumble" :compact="compact" v-bind="rumble" :x-label="t('ds5.plots.input')" :label="t('ds5.plots.rumble')" /><small v-if="!compact">{{ t('ds5.plots.rumble') }}</small></div>
      <div v-else-if="kind === 'left' || kind === 'right'" class="ds5-live__plot"><MDsPlot kind="response" :compact="compact" v-bind="stick(kind)" :x-label="t('ds5.plots.input')" :label="t('ds5.plots.stick_' + kind)" /><small v-if="!compact">{{ t('ds5.plots.stick_' + kind) }}</small></div>
      <div v-else-if="kind === 'axisLeft' || kind === 'axisRight'" class="ds5-live__plot"><MDsPlot kind="response" :compact="compact" v-bind="axis(kind === 'axisLeft' ? 'left' : 'right')" :x-label="t('ds5.plots.travel')" :label="t('ds5.plots.trigger_' + (kind === 'axisLeft' ? 'left' : 'right'))" /><small v-if="!compact">{{ t('ds5.plots.trigger_' + (kind === 'axisLeft' ? 'left' : 'right')) }}</small></div>
      <div v-else-if="kind === 'gyro'" class="ds5-live__plot"><MDsPlot kind="response" :compact="compact" v-bind="gyro" :x-label="t('ds5.plots.rate')" :label="t('ds5.plots.gyro')" /><small v-if="!compact">{{ t('ds5.plots.gyro') }}</small></div>
      <span v-else-if="kind === 'route'" class="ds5-live__route" :class="{ 'is-compact': compact }" role="img" :aria-label="t('ds5.plots.route')">
        <MIcon :name="route === 'headset' ? 'headphones' : 'speaker'" fill /><MIcon name="arrow_forward" /><MIcon name="sports_esports" />
      </span>
      <span v-else-if="kind === 'bar' && compact" class="ds5-live__swatch" :class="`is-${bar.mode}`" :style="bar.color ? { '--c': bar.color } : null" role="img" :aria-label="bar.label" />
      <div v-else-if="kind === 'bar'" class="ds5-live__plot ds5-live__plot--bar"><MLightbar :mode="bar.mode" :color="bar.color" :label="bar.label" /></div>
      <MDsPlot v-else-if="kind === 'players' && compact" kind="players" compact :lit="players" :label="t('ds5.plots.players')" class="ds5-live__dots" />
      <div v-else-if="kind === 'players'" class="ds5-live__plot ds5-live__plot--players"><MDsPlot kind="players" :lit="players" :label="t('ds5.plots.players')" /></div>
      <div v-else-if="kind === 'trigger' && compact" class="ds5-live__plot"><MDsPlot kind="response" compact :inner="0" :outer="1" :sensitivity="triggerOn ? Math.min(1, Number(edited[25] ?? 1)) : 0.04" :label="t('ds5.plots.trigger_left')" /></div>
      <TriggerPreview v-else-if="kind === 'trigger'" class="ds5-preview" :enabled="triggerOn" :strength="Number(edited[25] ?? 1)" :left-strength="Number(edited[26] ?? 1)" :right-strength="Number(edited[27] ?? 1)" />
    </template>
  </div>
</template>

<style>
.ds5-live { display: flex; flex-wrap: wrap; justify-content: center; gap: 12px 28px; padding: 16px 20px; border-radius: 28px; background: var(--m-card, var(--md-sys-color-surface-container-low)); }
.ds5-live--compact { flex-wrap: nowrap; gap: 8px; padding: 0; background: none; width: 132px; flex: none; }
.ds5-live__plot { display: flex; flex-direction: column; gap: 4px; min-width: 0; flex: 1 1 260px; max-width: 360px; }
.ds5-live--compact .ds5-live__plot { flex: 1; max-width: none; }
.ds5-live__plot small { font: var(--m-label-medium); color: var(--md-sys-color-on-surface-variant); text-align: center; }
.ds5-live__plot--bar { flex: 1 1 100%; max-width: none; }
.ds5-live__plot--players { flex: 1 1 100%; max-width: none; }
.ds5-live__plot--players svg { max-width: 260px; margin: 0 auto; }
.ds5-live .m-dsplot svg { max-height: 150px; }
.ds5-live__route { display: inline-flex; align-items: center; gap: 10px; color: var(--md-sys-color-primary-text); }
.ds5-live__route .m-icon { font-size: 32px; }
.ds5-live__route.is-compact .m-icon { font-size: 22px; }
.ds5-live__route.is-compact { color: var(--md-sys-color-on-surface-variant); }
.ds5-live--compact:has(.ds5-live__swatch) { flex-direction: column; align-items: flex-end; gap: 6px; }
.ds5-live__swatch { width: 64px; height: 22px; border-radius: 11px; background: var(--c, var(--md-sys-color-primary)); }
.ds5-live__swatch.is-game { background: linear-gradient(90deg, #c850c0, #ff6f61, #ffd700); }
.ds5-live__swatch.is-off { background: var(--md-sys-color-surface-container-highest); box-shadow: inset 0 0 0 1.5px var(--md-sys-color-outline-variant); }
.ds5-live--compact .ds5-live__dots { width: 64px; }
.ds5-live--compact .ds5-live__dots svg { max-height: 12px; }
.ds5-live .ds5-preview { flex: 1 1 100%; }
</style>
