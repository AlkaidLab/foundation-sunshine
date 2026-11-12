<script setup>
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { MRoute, sessionClientLabel } from 'foundation-ui/vue'
import { encoderName } from 'foundation-ui/settings/index.js'
import { deviceName } from '../../../utils/audioDevices.js'

// The video and the audio path of a stream as pictures: what the settings below decide, lit by the host's own state.
// Everything shown is read from the configuration or from the host (the session in progress, the microphone route);
// a node whose switch is off is dimmed, a path with no session does not flow.
const props = defineProps({
  config: { type: Object, required: true },
  platform: { type: String, default: 'windows' },
  kind: { type: String, default: 'video' },
  session: { type: Object, default: null },
  perf: { type: Object, default: null },
  mic: { type: Object, default: null },
  /** the playback endpoints of the host and the VB-CABLE summary (utils/audioDevices.js): names instead of stored ids */
  devices: { type: Array, default: () => [] },
  vb: { type: Object, default: null },
})
const { t, te } = useI18n()
const on = (key, def = true) => { const v = props.config[key]; return v === undefined || v === '' ? def : v === true || v === 'true' || v === 'enabled' }
const mbps = (kbps) => `${Math.round(Number(kbps) / 100) / 10}`.replace(/\.0$/, '')
const sizes = computed(() => (Array.isArray(props.config.resolutions) ? props.config.resolutions : String(props.config.resolutions || '').replace(/[[\]\s]/g, '').split(',').filter(Boolean)))
const biggest = computed(() => sizes.value.map((s) => String(s).split('x').map(Number)).filter((p) => p.length === 2 && p[0] > 0).sort((a, b) => b[0] * b[1] - a[0] * a[1])[0])
const displayName = computed(() => {
  const out = props.config.output_name
  if (out === 'ZakoHDR') return t('config.output_name_vdd_option')
  const device = (props.config.display_devices || []).find((d) => d.device_id === out)
  if (device) return String(device.data || '').replace(/.*?(DISPLAY\d+)?\nFRIENDLY NAME: (.*[^\n])*?\n.*\n.*/g, '$2').trim() || out
  return out ? String(out) : t('config.route_display_auto')
})
const codecText = computed(() => (props.session?.codec ? String(props.session.codec).toUpperCase() : ['H.264', Number(props.config.hevc_mode) > 0 ? 'HEVC' : '', Number(props.config.av1_mode) > 0 ? 'AV1' : ''].filter(Boolean).join(' · ')))
const bitrateKbps = computed(() => props.perf?.bitrate_kbps ?? props.session?.bitrate ?? 0)
const live = computed(() => !!props.session)
const client = computed(() => ({ icon: 'devices', title: sessionClientLabel(props.session) || t('config.route_no_client'), state: props.session ? 'on' : 'off' }))

const video = computed(() => [
  { id: 'display', icon: 'monitor', title: displayName.value, text: props.session?.width ? `${props.session.width}×${props.session.height} · ${props.session.fps} FPS` : biggest.value ? t('config.route_up_to', { size: `${biggest.value[0]}×${biggest.value[1]}` }) : '' },
  { id: 'encoder', icon: 'memory', title: encoderName(props.config.active_encoder) || t('_common.auto'), text: codecText.value },
  { id: 'stream', icon: 'cast_connected', title: bitrateKbps.value ? `${mbps(bitrateKbps.value)} Mbps` : Number(props.config.max_bitrate) > 0 ? t('config.route_bitrate_cap', { mbps: mbps(props.config.max_bitrate) }) : t('config.route_bitrate_auto'), text: Number(props.config.port) > 0 ? `${t('config.port')} ${props.config.port}` : '' },
  { id: 'client', ...client.value },
])
const micState = computed(() => {
  const m = props.mic
  if (!on('stream_mic')) return 'off'
  if (m?.error_code) return 'warn'
  return m?.online ? 'on' : 'off'
})
const defaultDevice = computed(() => props.devices.find((d) => d.isDefault && d.state === 'active')?.name || '')
// the system default named when no device is chosen; the device that takes the sound while a stream runs, by its name
const audioDeviceText = computed(() => {
  if (props.config.virtual_sink) return t('config.route_virtual', { name: deviceName(props.devices, props.config.virtual_sink) })
  return props.config.audio_sink ? '' : defaultDevice.value
})
const audio = computed(() => [
  { id: 'apps', icon: 'sports_esports', title: t('config.route_apps') },
  { id: 'device', icon: 'speaker', title: props.config.audio_sink ? deviceName(props.devices, props.config.audio_sink) : t('config.route_device_default'), text: audioDeviceText.value },
  { id: 'stream', icon: 'graphic_eq', title: t('config.stream_audio'), state: on('stream_audio') ? 'on' : 'off' },
  { id: 'client', ...client.value },
])
const micBackend = computed(() => props.mic?.active_backend || props.config.microphone_redirect_backend || 'auto')
// the backend by its name, never by its id
const micBackendTitle = computed(() => (te(`config.route_mic_backend_${micBackend.value}`) ? t(`config.route_mic_backend_${micBackend.value}`) : String(micBackend.value)))
const vbTrouble = computed(() => micBackend.value === 'vb_cable' && ['missing', 'unavailable', 'partial'].includes(props.vb?.state))
const microphone = computed(() => [
  { id: 'client', icon: 'mic', title: t('config.route_mic_client'), state: on('stream_mic') ? 'on' : 'off' },
  { id: 'backend', icon: 'swap_horiz', title: micBackendTitle.value, text: vbTrouble.value ? t(`config.vb_cable_state_${props.vb.state}`) : props.mic ? t(`config.microphone_redirect_state_${props.mic.state || 'absent'}`) : '', ...(vbTrouble.value ? { state: 'warn' } : {}) },
  { id: 'device', icon: 'settings_input_hdmi', title: t('config.route_mic_device'), state: props.mic?.device_created ? 'on' : micState.value === 'off' ? 'off' : 'on' },
  { id: 'apps', icon: 'apps', title: t('config.route_mic_apps'), state: micState.value },
])
</script>

<template>
  <MRoute v-if="kind === 'video'" :nodes="video" :label="$t('config.av_video_title')" :flow="live" data-route="video" />
  <template v-else>
    <MRoute :nodes="audio" :label="$t('config.av_audio_title')" :flow="live && on('stream_audio')" data-route="audio" />
    <MRoute v-if="platform === 'windows'" :nodes="microphone" :label="$t('config.microphone_redirect_backend')" :flow="live && micState === 'on'" data-route="mic" />
  </template>
</template>
