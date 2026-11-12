<script setup>
import { computed, inject, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../platform-i18n'
import { openDeviceManager, openExternalUrl } from '../../utils/helpers.js'
import { apiJson, apiPostJson } from '../../utils/apiFetch.js'
import { MBanner, MButton, MChip, MFooter, MGroup, MItem, MSection, MStatusPills } from 'foundation-ui/vue'
import AvRoutes from './audiovideo/AvRoutes.vue'
import CfgDevice from '../../components/cfg/CfgDevice.vue'
import VbCableRow from '../../components/cfg/VbCableRow.vue'
import { useSettingsLive } from '../../composables/useSettingsLive.js'
import { useAudioDevices } from '../../composables/useAudioDevices.js'
import { vbCableReady } from '../../utils/audioDevices.js'
import CfgChoice from '../../components/cfg/CfgChoice.vue'
import CfgField from '../../components/cfg/CfgField.vue'
import CfgSub from '../../components/cfg/CfgSub.vue'
import CfgSwitch from '../../components/cfg/CfgSwitch.vue'
import ConfirmDialog from '../../components/common/ConfirmDialog.vue'
import VddPrerequisiteNotice from '../../components/common/VddPrerequisiteNotice.vue'

const props = defineProps(['platform', 'config', 'resolutions', 'fps'])

const { t, te } = useI18n()
const config = ref(props.config)
const cfgNav = inject('cfgNav')
const live = useSettingsLive({ platform: props.platform, want: ['session', 'bitrate'] })
const audio = useAudioDevices(props.platform)
const showDownloadConfirm = ref(false)
const micTestRunning = ref(false)
const micTestResult = ref(null)
const micStatusLoading = ref(false)
const micStatus = ref(null)
const micLevelShown = ref(0)
let micStatusTimer
let micStatusPending = false
let micStatusActive = false
let micStatusRetry = null
let micLevelTimer

const microphoneBackend = computed(() => config.value.microphone_redirect_backend || 'auto')
const showUsbipStatus = computed(() => ['usbip_experimental', 'auto'].includes(microphoneBackend.value))
const micStatusTone = computed(() => {
  if (!micStatus.value) return 'neutral'
  if (micStatus.value.online && micStatus.value.device_created) return 'ok'
  if (micStatus.value.error_code) return 'error'
  return 'neutral'
})
/** A stream delivered client microphone frames at least once: the live meter is meaningful then. */
const micLiveInput = computed(() => Number(micStatus.value?.input_frames) > 0)
const micLevelFresh = computed(() => {
  const ms = Number(micStatus.value?.input_active_ms)
  return Number.isFinite(ms) && ms >= 0 && ms < 3000
})
// the backend by its name, never by its id
const backendName = (id) => (id && te(`config.route_mic_backend_${id}`) ? t(`config.route_mic_backend_${id}`) : id || '—')
const yes = (value) => t(value ? 'config.microphone_redirect_available' : 'config.microphone_redirect_unavailable')
// the status as labelled pills: what is available and what the host does now; the three counters only warn when one is not zero
const micPills = computed(() => {
  const s = micStatus.value
  if (!s) return []
  const counters = [s.underruns || 0, s.dropped_frames || 0, s.submit_errors || 0]
  return [
    { id: 'backend', icon: 'settings_voice', label: t('config.microphone_redirect_active_backend'), value: backendName(s.active_backend) },
    { id: 'component', icon: 'extension', label: t('config.microphone_redirect_component'), value: yes(s.component_available), tone: s.component_available ? 'ok' : 'off' },
    { id: 'endpoint', icon: 'mic', label: t('config.microphone_redirect_endpoint'), value: yes(s.device_created), tone: s.device_created ? 'ok' : 'off' },
    { id: 'capture', icon: 'graphic_eq', label: t('config.microphone_redirect_host_capture'), value: t(s.host_streaming ? 'config.microphone_redirect_active' : 'config.microphone_redirect_inactive'), tone: s.host_streaming ? 'live' : '' },
    { id: 'buffer', icon: 'memory', label: t('config.microphone_redirect_buffered'), value: `${s.buffered_bytes || 0} B` },
    { id: 'diag', icon: counters.some(Boolean) ? 'warning' : 'check_circle', label: t('config.microphone_redirect_diagnostics'), value: counters.join(' / '), tone: counters.some(Boolean) ? 'warn' : '' },
  ]
})
// every way the host offers, side by side: short names on the buttons, what each does in the line below
const micBackends = computed(() => ['auto', 'usbip_experimental', 'vb_cable', 'disabled'].map((value) => ({ value, label: t(`config.route_mic_backend_${value}`) })))
const micBackendText = computed(() => t({ auto: 'config.microphone_redirect_backend_auto', usbip_experimental: 'config.microphone_redirect_backend_usbip', vb_cable: 'config.microphone_redirect_backend_vb', disabled: 'config.microphone_redirect_backend_disabled' }[microphoneBackend.value] || 'config.microphone_redirect_backend_auto'))

const refreshMicrophoneStatus = async ({ quiet = false, afterPending = false } = {}) => {
  if (props.platform !== 'windows' || document.hidden || !micStatusActive) return
  if (micStatusPending) {
    if (!quiet || afterPending) micStatusRetry = micStatusRetry === false ? false : quiet
    return
  }
  micStatusPending = true
  if (!quiet) micStatusLoading.value = true
  try {
    const status = await apiJson('/api/microphone/status')
    if (micStatusActive) micStatus.value = status
  } catch {
    if (micStatusActive && !quiet) micStatus.value = null
  } finally {
    micStatusPending = false
    if (!quiet) micStatusLoading.value = false
    if (micStatusRetry !== null) {
      const quiet = micStatusRetry
      micStatusRetry = null
      void refreshMicrophoneStatus({ quiet })
    }
  }
}

const handleOpenDevices = async () => {
  try {
    await openDeviceManager()
  } catch (error) {
    console.error('Failed to open the Device Manager:', error)
  }
}

const handleDownloadVSink = () => {
  showDownloadConfirm.value = true
}

const confirmDownload = async () => {
  showDownloadConfirm.value = false
  const url = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip'

  try {
    await openExternalUrl(url)
  } catch (error) {
    console.error('Failed to open URL:', error)
  }
}

const cancelDownload = () => {
  showDownloadConfirm.value = false
}

const testMicrophoneRoute = async () => {
  micTestRunning.value = true
  micTestResult.value = null

  try {
    const result = await apiPostJson('/api/microphone/test')
    micTestResult.value = {
      success: result.success === true,
      backend: result.backend || '',
      errorCode: result.error_code || '',
      messageKey: result.success === true
        ? (result.mode === 'live'
            ? 'config.stream_mic_test_success_live'
            : result.backend === 'usbip_experimental'
            ? 'config.stream_mic_test_success_usbip'
            : 'config.stream_mic_test_success')
        : (
            result.error_code === 'MIC_TEST_DEVICE_UNAVAILABLE'
              ? 'config.stream_mic_test_device_unavailable'
              : result.error_code === 'MIC_USBIP_COMPONENT_UNAVAILABLE'
                ? 'config.stream_mic_test_usbip_unavailable'
              : 'config.stream_mic_test_failed'
          ),
    }
  } catch {
    micTestResult.value = {
      success: false,
      messageKey: 'config.stream_mic_test_failed',
    }
  } finally {
    micTestRunning.value = false
    await refreshMicrophoneStatus({ quiet: true, afterPending: true })
  }
}

watch(microphoneBackend, () => {
  micTestResult.value = null
})

watch(() => micStatus.value?.input_peak, (peak) => {
  const value = Number(peak) || 0
  if (value > micLevelShown.value) micLevelShown.value = Math.min(1, value)
})

const syncMicrophonePolling = () => {
  window.clearInterval(micStatusTimer)
  micStatusTimer = undefined
  if (!micStatusActive || document.hidden || props.platform !== 'windows') return
  void refreshMicrophoneStatus({ quiet: micStatus.value !== null })
  micStatusTimer = window.setInterval(() => refreshMicrophoneStatus({ quiet: true }), 3000)
}

onMounted(() => {
  micStatusActive = true
  syncMicrophonePolling()
  document.addEventListener('visibilitychange', syncMicrophonePolling)
  // Peak-hold with decay: a poll every 3 s must still render a visible bump per spoken phrase.
  micLevelTimer = window.setInterval(() => {
    micLevelShown.value = Math.max(0, micLevelShown.value - 0.04)
  }, 250)
})

onBeforeUnmount(() => {
  micStatusActive = false
  micStatusRetry = null
  window.clearInterval(micStatusTimer)
  window.clearInterval(micLevelTimer)
  document.removeEventListener('visibilitychange', syncMicrophonePolling)
})
</script>

<template>
  <CfgSub id="sound" :title="$t('shell.audio_output')">
    <MSection>
      <AvRoutes kind="audio" :config="config" :platform="platform" :session="live.sessions.value[0]" :perf="live.perf.value" :mic="micStatus" :devices="audio.render.value" :vb="audio.vb.value" />
      <MGroup>
        <CfgSwitch :config="config" k="stream_audio" :on="true" :off="false" def="true" />
        <CfgDevice v-if="platform === 'windows'" :config="config" k="audio_sink" tp :devices="audio.render.value" :placeholder="$tp('config.audio_sink_placeholder')" />
        <CfgField v-else :config="config" k="audio_sink" tp :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')" />
        <template v-if="platform === 'windows'">
          <CfgDevice :config="config" k="virtual_sink" :devices="audio.render.value" :placeholder="$t('config.virtual_sink_placeholder')" />
          <CfgSwitch :config="config" k="keep_sink_default" :on="true" :off="false" def="false" />
          <CfgSwitch :config="config" k="install_steam_audio_drivers" def="enabled" />
          <VbCableRow v-if="audio.loaded.value && !vbCableReady(audio.vb.value)" :vb="audio.vb.value" :loaded="audio.loaded.value" @download="handleDownloadVSink" @open-devices="handleOpenDevices" @changed="audio.refresh" />
        </template>
      </MGroup>
      <MFooter v-if="platform !== 'windows'">
        {{ $tp('config.audio_sink_desc') }}
        <template v-if="platform === 'linux'"><code>pacmd list-sinks | grep "name:"</code> <code>pactl info | grep Source</code></template>
        <template v-else-if="platform === 'macos'"><a href="https://github.com/mattingalls/Soundflower" target="_blank" rel="noopener noreferrer">Soundflower</a>, <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank" rel="noopener noreferrer">BlackHole</a></template>
      </MFooter>
    </MSection>
  </CfgSub>

  <CfgSub id="microphone" :title="$t('shell.microphone')">
    <MSection>
      <MGroup>
        <CfgSwitch :config="config" k="stream_mic" :on="true" :off="false" def="true" />
        <CfgChoice v-if="platform === 'windows'" :config="config" k="microphone_redirect_backend" :options="micBackends" def="auto" />
        <VbCableRow v-if="platform === 'windows'" :vb="audio.vb.value" :loaded="audio.loaded.value" @download="handleDownloadVSink" @open-devices="handleOpenDevices" @changed="audio.refresh" />
        <MItem v-else action icon="download" :title="$t('_common.download')" @click="handleDownloadVSink" />
        <MItem v-if="platform === 'windows'" action icon="graphic_eq" :title="micTestRunning ? $t('config.stream_mic_testing') : $t('config.stream_mic_test')" :disabled="micTestRunning || microphoneBackend === 'disabled'" @click="testMicrophoneRoute" />
      </MGroup>
      <MFooter>{{ $t('config.stream_mic_test_how') }}</MFooter>
      <MBanner v-if="platform === 'windows' && showUsbipStatus" tone="info" icon="info">{{ $t('config.microphone_redirect_usbip_warning') }}</MBanner>
      <MBanner v-if="micTestResult" :tone="micTestResult.success ? 'info' : 'error'" :icon="micTestResult.success ? 'check_circle' : 'error'">
        {{ $t(micTestResult.messageKey) }}<code v-if="micTestResult.errorCode"> {{ micTestResult.errorCode }}</code>
      </MBanner>
      <MFooter>{{ platform === 'windows' ? $t(showUsbipStatus ? 'config.stream_mic_test_note_usbip' : 'config.stream_mic_test_note') : $t('config.stream_mic_note') }}</MFooter>
    </MSection>

    <MSection v-if="platform === 'windows'" :title="$t('config.microphone_redirect_status')">
      <template #action>
        <MChip :tone="micStatusTone">{{ micStatus ? $t(`config.microphone_redirect_state_${micStatus.state || 'absent'}`) : $t('config.microphone_redirect_state_unknown') }}</MChip>
        <MButton variant="text" size="sm" icon="refresh" :loading="micStatusLoading" @click="refreshMicrophoneStatus()">{{ $t('config.microphone_redirect_refresh') }}</MButton>
      </template>
      <MStatusPills v-if="micPills.length" :items="micPills" :label="$t('config.microphone_redirect_status')" />
      <div v-if="micLiveInput" :aria-label="$t('config.microphone_redirect_input_level')" style="display:flex;align-items:center;gap:12px;margin:6px 0 2px;">
        <span style="font-size:0.85em;opacity:0.8">{{ $t('config.microphone_redirect_input_level') }}</span>
        <span style="flex:1;height:8px;border-radius:4px;background:rgba(128,128,128,0.25);overflow:hidden;">
          <i :style="{ width: `${Math.round(micLevelShown * 100)}%` }" style="display:block;height:100%;background:#4c8bf5;transition:width 240ms linear;" />
        </span>
        <small style="opacity:0.75">{{ $t(micLevelFresh ? 'config.microphone_redirect_input_active' : 'config.microphone_redirect_input_silent') }}</small>
      </div>
      <MFooter v-if="micStatus?.fallback_reason">{{ $t('config.microphone_redirect_fallback') }}: <code>{{ micStatus.fallback_reason }}</code></MFooter>
      <MFooter v-if="micStatus?.error_code">{{ $t('config.microphone_redirect_error') }}: <code>{{ micStatus.error_code }}</code></MFooter>
      <MFooter>{{ $t('config.microphone_redirect_apply_note') }}</MFooter>
    </MSection>
  </CfgSub>


  <ConfirmDialog
    :show="showDownloadConfirm"
    dialog-id="stream-mic-download-confirm"
    :title="$t('_common.download')"
    title-icon="open_in_new"
    :close-label="$t('_common.close')"
    @close="cancelDownload"
  >
    <p>{{ $t('config.stream_mic_download_confirm') }}</p>
    <template #actions>
      <MButton variant="text" @click="cancelDownload">{{ $t('_common.cancel') }}</MButton>
      <MButton icon="download" @click="confirmDownload">{{ $t('_common.download') }}</MButton>
    </template>
  </ConfirmDialog>
</template>
