<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MStatusPills } from 'foundation-ui/vue'
import { encoderName } from 'foundation-ui/settings/index.js'
import { codecRows } from '../../../utils/codecMatrix.js'

// The status of the encoder as one slim line (the encoder page; the bitrate cap is the first section of that page and HDR has its own page, so neither repeats here): the encoder in use, the load of the GPU and of the CPU, and for each codec a small
// chip with its state (offered, offered with 10-bit, left to the host, not offered, in use now). Everything is read: the encoder
// the host reports, the host's counters (the numbers are live), the encoder probe and the session in progress. The line never wraps;
// at narrow widths the labels give way to the icons.
const props = defineProps({
  config: { type: Object, required: true },
  session: { type: Object, default: null },
  system: { type: Object, default: null },
})
const { t } = useI18n()
const pct = (v) => `${Math.round(Number(v))}%`
const state = (row) => {
  if (row.now) return { icon: 'play_circle', tone: 'live', value: t('config.matrix_now') }
  if (row.offered === 'no') return { icon: 'remove', tone: 'off', value: '' }
  if (row.offered === 'auto') return { icon: 'bolt', tone: 'auto', value: '' }
  return { icon: 'check_circle', tone: 'ok', value: row.deep === 'yes' ? '10-bit' : '' }
}
const items = computed(() => [
  { id: 'encoder', icon: 'memory', label: t('config.encoder_short'), optionalLabel: true, value: encoderName(props.config.active_encoder) || t('_common.auto'), tone: props.config.active_encoder ? 'ok' : 'off' },
  ...(props.system?.gpu_percent != null ? [{ id: 'gpu', icon: 'speed', label: 'GPU', value: pct(props.system.gpu_percent), optionalLabel: true, title: props.system.gpu_name || '' }] : []),
  ...(props.system?.cpu_percent != null ? [{ id: 'cpu', icon: 'speed', label: 'CPU', value: pct(props.system.cpu_percent), optionalLabel: true }] : []),
  ...codecRows(props.config, props.session?.codec).map((row) => ({ id: row.id, label: row.name, optionalValue: true, ...state(row) })),
])
</script>

<template>
  <section class="cfg-matrix" data-matrix="encoder"><MStatusPills :items="items" :label="$t('config.matrix_title')" /></section>
</template>

<style>
.cfg-matrix { display: block; }
/* information, not a state to act on: one neutral style for every pill (the icon says the state), no accent fills */
.cfg-matrix .m-pill, .cfg-matrix .m-pill--live, .cfg-matrix .m-pill--ok, .cfg-matrix .m-pill--auto, .cfg-matrix .m-pill--off { background: var(--md-sys-color-surface-container-high); color: var(--md-sys-color-on-surface-variant); opacity: 1; }
.cfg-matrix .m-pill .m-pill__value, .cfg-matrix .m-pill .m-icon { color: var(--md-sys-color-on-surface-variant); }
</style>
