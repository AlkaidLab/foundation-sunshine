<!-- Colour state uses the WHATWG HTML Living Standard colour input
     (ds5-whatwg-input-20261031); R/G/B remain three separate profile keys. -->
<template>
  <MItem v-if="mode === 'fixed'" :title="$t('ds5.color.label')" stack class="ds5-color-row">
    <template #trail>
      <span class="ds5-chip-dot" :style="{ '--dot': hex }" aria-hidden="true"></span>
      <input class="ds5-hex m-input" type="text" :value="hex" maxlength="7" spellcheck="false" :aria-label="$t('ds5.color.hex')" @change="onHexInput" />
    </template>
    <template #control>
      <div class="ds5-color-tools" role="group" :aria-label="$t('ds5.color.label')">
        <MSwatches :model-value="hex" :colors="swatchColors" :label="$t('ds5.color.label')" @update:model-value="applyHex" />
        <label class="m-swatch ds5-swatch-custom" :class="{ 'is-selected': !isSwatch }" :style="{ '--swatch': hex }" :data-tip="$t('ds5.color.picker')">
          <input type="color" :value="hex" :aria-label="$t('ds5.color.picker')" @input="onColorInput" />
          <MIcon :name="isSwatch ? 'palette' : 'check'" />
        </label>
      </div>
    </template>
  </MItem>
  <MItem v-else :title="$t('ds5.color.label')" :value="mode === 'off' ? $t('ds5.color.is_off') : $t('ds5.color.by_game')" class="ds5-color-row">
    <template #trail><span class="ds5-chip-dot" :class="{ off: mode === 'off', game: mode !== 'off' }" aria-hidden="true"></span></template>
  </MItem>
</template>

<script setup>
import { MIcon, MItem, MSwatches } from 'foundation-ui/vue'
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'

const props = defineProps({
  r: { type: Number, default: 0 },
  g: { type: Number, default: 0 },
  b: { type: Number, default: 0 },
  mode: { type: String, default: 'followGame' },
})

const emit = defineEmits(['update'])
const { t } = useI18n()

const swatches = Object.freeze([
  { name: 'blue', hex: '#4a9eff' },
  { name: 'red', hex: '#ff5252' },
  { name: 'green', hex: '#34c759' },
  { name: 'orange', hex: '#ff9f0a' },
  { name: 'violet', hex: '#af52de' },
  { name: 'white', hex: '#ffffff' },
])

const swatchColors = computed(() => swatches.map(swatch => ({ value: swatch.hex, label: t('ds5.color.swatch', { name: t('ds5.color.' + swatch.name) }) })))
const clamp = (value) => Math.min(255, Math.max(0, Number(value) || 0))
const hex = computed(() =>
  '#' + [props.r, props.g, props.b].map(value => clamp(value).toString(16).padStart(2, '0')).join(''))
const isSwatch = computed(() => swatches.some(swatch => swatch.hex === hex.value))

const applyHex = (value) => {
  emit('update', {
    r: parseInt(value.slice(1, 3), 16),
    g: parseInt(value.slice(3, 5), 16),
    b: parseInt(value.slice(5, 7), 16),
  })
}

const onColorInput = (event) => applyHex(event.target.value)

const onHexInput = (event) => {
  const value = event.target.value.trim().toLowerCase()
  const normalised = value.startsWith('#') ? value : '#' + value
  if (!/^#[0-9a-f]{6}$/.test(normalised)) {
    event.target.value = hex.value
    return
  }
  applyHex(normalised)
}
</script>

<style>
.ds5-color-tools { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; }
.ds5-swatch-custom { position: relative; display: grid; place-items: center; cursor: pointer; color: #fff; mix-blend-mode: normal; }
.ds5-swatch-custom .m-icon { font-size: 18px; pointer-events: none; filter: drop-shadow(0 0 2px rgb(0 0 0 / 0.6)); }
.ds5-swatch-custom.is-selected { border-radius: 10px; }
.ds5-swatch-custom input { position: absolute; inset: 0; opacity: 0; width: 100%; height: 100%; cursor: pointer; }
.ds5-hex { width: 88px; height: 36px; padding: 0 12px; border-radius: 12px; font: var(--m-label-large); font-variant-numeric: tabular-nums; text-transform: lowercase; }
.ds5-chip-dot { width: 24px; height: 24px; border-radius: 50%; background: var(--dot, transparent); box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--md-sys-color-on-surface) 18%, transparent); }
.ds5-chip-dot.game { background: conic-gradient(#ff5252, #ff9f0a, #34c759, #4a9eff, #af52de, #ff5252); }
.ds5-chip-dot.off { background: var(--md-sys-color-surface-container-highest); }
</style>
