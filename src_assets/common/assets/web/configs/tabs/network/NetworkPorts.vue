<script setup>
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MChart, MFooter, MIcon } from 'foundation-ui/vue'
import { checkHttpPort, hostPorts } from '../../../utils/portCheck.js'
import { useSettingsLive } from '../../../composables/useSettingsLive.js'

// The ports of the host as chips (what each is for, whether the host reports it bound, whether this device reached it)
// and the bitrate of the stream in progress as a curve against the ceiling. A port check is real: it is an HTTP request
// from this device, so it can only try the plain HTTP port; the others say why they are not tried.
const props = defineProps({ config: { type: Object, required: true }, base: { type: Number, required: true }, listener: { type: Object, default: null } })
const { t } = useI18n()
const live = useSettingsLive({ platform: 'windows', want: ['session', 'bitrate'] })
const ports = computed(() => hostPorts(props.base).map((p) => ({ ...p, note: p.note === 'web' ? t('config.port_web_ui') : p.note === 'stream' ? t('config.network_stream_ports') : p.note })))
const results = ref({})
const checking = ref(false)
const host = typeof location !== 'undefined' ? location.hostname : 'localhost'
async function run() {
  checking.value = true
  results.value = {}
  const next = {}
  for (const p of ports.value) {
    if (p.check === true) next[p.id] = await checkHttpPort(host, p.port)
    else if (p.check === 'self') next[p.id] = String(location.port) === p.port ? 'reachable' : 'untried'
    else next[p.id] = 'untried'
  }
  results.value = next
  checking.value = false
}
const bound = (p) => p.protocol === 'TCP' && props.listener && String(props.listener.port) === String(props.base) && ['http', 'https', 'web', 'rtsp'].includes(p.id)
const stateOf = (p) => results.value[p.id] || ''
const ICON = { reachable: 'check_circle', unreachable: 'error', untried: 'help', '': 'radio_button_unchecked' }
const hint = (p) => (stateOf(p) === 'untried' ? t(p.protocol === 'UDP' ? 'config.port_try_udp' : 'config.port_try_tls') : '')

// bitrate of the session in progress, one sample every two seconds, the last four minutes
const samples = ref([])
let last = 0
watch(() => [live.perf.value, live.sessions.value], () => {
  const now = Date.now()
  if (now - last < 1800) return
  last = now
  const kbps = live.perf.value?.bitrate_kbps ?? live.sessions.value[0]?.bitrate
  if (!live.sessions.value.length) { samples.value = []; return }
  if (Number.isFinite(Number(kbps))) samples.value = [...samples.value, Number(kbps) / 1000].slice(-120)
}, { deep: true })
const cap = computed(() => Number(props.config.max_bitrate || 0) / 1000)
onBeforeUnmount(() => { samples.value = [] })
</script>

<template>
  <section class="cfg-ports" data-ports="host" :aria-label="$t('config.network_port_details')">
    <ul class="cfg-ports__list">
      <li v-for="p in ports" :key="p.id" class="cfg-ports__chip" :class="[`is-${stateOf(p) || 'idle'}`, { 'is-bound': bound(p) }]" :title="hint(p)">
        <MIcon :name="ICON[stateOf(p)]" :fill="stateOf(p) === 'reachable'" />
        <span class="cfg-ports__num"><b>{{ p.protocol }} {{ p.port }}</b><small>{{ p.note }}<template v-if="bound(p)"> · {{ $t('config.port_listening') }}</template></small></span>
      </li>
    </ul>
    <div class="cfg-ports__bar">
      <MButton variant="tonal" size="sm" icon="network_check" :loading="checking" @click="run">{{ $t('config.port_check') }}</MButton>
      <small v-if="Object.values(results).includes('untried')">{{ $t('config.port_check_note') }}</small>
    </div>
    <MChart v-if="samples.length > 1" :points="samples" unit=" Mbps" :step="2" :max="cap > 0 ? cap : null" :height="140" :label="$t('config.bitrate_chart')" />
    <div v-else class="cfg-ports__idle" role="status"><MIcon name="speed" /><span><b>{{ $t('config.bitrate_chart') }}</b><small>{{ $t('config.bitrate_chart_empty') }}<template v-if="cap > 0"> · {{ $t('config.route_bitrate_cap', { mbps: cap }) }}</template></small></span></div>
    <MFooter>{{ $t('config.port_web_ui_proxy_warning') }}</MFooter>
  </section>
</template>

<style>
.cfg-ports { display: flex; flex-direction: column; gap: 12px; padding: 16px 20px; border-radius: 28px; background: var(--m-card); }
.cfg-ports__list { list-style: none; margin: 0; padding: 0; display: flex; flex-wrap: wrap; gap: 8px; }
.cfg-ports__chip { display: inline-flex; align-items: center; gap: 8px; padding: 6px 14px 6px 10px; border-radius: 16px; background: var(--md-sys-color-surface-container-highest); color: var(--md-sys-color-on-surface-variant); transition: background-color var(--m-spring-default-effects-duration) var(--m-spring-default-effects), border-radius var(--m-spring-fast-spatial-duration) var(--m-spring-fast-spatial); }
.cfg-ports__chip .m-icon { font-size: 20px; }
.cfg-ports__num { display: flex; flex-direction: column; line-height: 1.2; }
.cfg-ports__num b { font: var(--m-label-large); color: var(--md-sys-color-on-surface); font-variant-numeric: tabular-nums; }
.cfg-ports__num small { font: var(--m-body-small); }
.cfg-ports__chip.is-reachable { background: var(--md-sys-color-success-container, var(--md-sys-color-secondary-container)); color: var(--md-sys-color-on-success-container, var(--md-sys-color-on-secondary-container)); border-radius: 22px; }
.cfg-ports__chip.is-unreachable { background: var(--md-sys-color-error-container); color: var(--md-sys-color-on-error-container); }
.cfg-ports__idle { display: flex; align-items: center; gap: 12px; padding: 10px 14px; border-radius: 16px; background: color-mix(in srgb, var(--md-sys-color-on-surface) 5%, transparent); color: var(--md-sys-color-on-surface-variant); }
.cfg-ports__idle .m-icon { font-size: 22px; color: var(--md-sys-color-primary-text); }
.cfg-ports__idle span { display: flex; flex-direction: column; line-height: 1.25; }
.cfg-ports__idle b { font: var(--m-label-large); color: var(--md-sys-color-on-surface); }
.cfg-ports__idle small { font: var(--m-body-small); }
.cfg-ports__bar { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.cfg-ports__bar small { font: var(--m-body-small); color: var(--md-sys-color-on-surface-variant); }
</style>
