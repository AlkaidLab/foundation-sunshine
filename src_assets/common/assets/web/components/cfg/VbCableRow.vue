<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MButton, MItem } from 'foundation-ui/vue'
import ConfirmDialog from '../common/ConfirmDialog.vue'
import { apiPostJson } from '../../utils/apiFetch.js'
import { isTauriEnv } from '../../utils/helpers.js'
import { vbCableErrorKind, vbCableRunningStep } from '../../utils/audioDevices.js'

// VB-CABLE as the host sees it: ready, partly usable, disabled (installed, its device switched off in the Device Manager),
// unavailable (installed, unusable for another reason) or not installed. The row does what the original's script
// install-vsink.bat did, but on request and with a check: the host downloads the driver pack from VB-Audio, verifies the
// signature of its setup program and installs it; a disabled device is enabled with pnputil. Both ask first.
// The download link of the original stays as the manual way; the Device Manager opens only inside the control panel
// (a browser cannot open a program on the host).
const props = defineProps({
  /** { state, job } of utils/audioDevices.js normalizeAudioDevices */
  vb: { type: Object, required: true },
  loaded: { type: Boolean, default: false },
})
const emit = defineEmits(['download', 'open-devices', 'changed'])
const { t } = useI18n()
const state = computed(() => (['ready', 'partial', 'disabled', 'unavailable', 'missing'].includes(props.vb.state) ? props.vb.state : 'unknown'))
const job = computed(() => props.vb.job || {})
const running = computed(() => vbCableRunningStep(job.value))
const requested = ref(false)
const busy = computed(() => !!running.value || requested.value)
const requestFailed = ref(false)
const failure = computed(() => {
  if (requestFailed.value) return 'failed'
  // the verdict of the last job counts only while the device is still not usable
  return props.vb.state !== 'ready' && job.value.step === 'failed' ? vbCableErrorKind(job.value.errorCode) : ''
})
const text = computed(() => {
  if (!props.loaded) return ''
  if (running.value) return t(`config.vb_cable_job_${running.value}`)
  if (failure.value) return t(`config.vb_cable_err_${failure.value}`)
  return t(`config.vb_cable_state_${state.value}`)
})
const primary = computed(() => {
  if (!props.loaded || state.value === 'ready' || state.value === 'unknown') return ''
  return state.value === 'disabled' ? 'enable' : 'install'
})
const primaryIcon = computed(() => (primary.value === 'enable' ? 'power_settings_new' : 'download'))
const showDevices = computed(() => props.loaded && state.value === 'disabled' && isTauriEnv())
const showDownload = computed(() => props.loaded && ['missing', 'unavailable', 'partial'].includes(state.value))

const asking = ref('')
const start = async () => {
  const action = asking.value
  asking.value = ''
  requested.value = true
  requestFailed.value = false
  try {
    const answer = await apiPostJson(`/api/audio/vb-cable/${action}`, { confirm: true })
    if (answer?.success !== true && answer?.error_code !== 'VB_BUSY') requestFailed.value = true
  } catch {
    requestFailed.value = true
  } finally {
    requested.value = false
    emit('changed')
  }
}
</script>

<template>
  <MItem data-vb-cable :data-vb-state="props.loaded ? state : 'loading'" :data-vb-failure="failure || undefined" icon="cable" :title="t('config.vb_cable_title')" :text="text" wrap>
    <template v-if="primary" #trail>
      <MButton v-if="showDevices" size="sm" variant="text" icon="open_in_new" :disabled="busy" @click="emit('open-devices')">{{ t('config.vb_cable_open_devmgmt') }}</MButton>
      <MButton v-if="showDownload" size="sm" variant="text" icon="download" :disabled="busy" @click="emit('download')">{{ t('_common.download') }}</MButton>
      <MButton size="sm" variant="tonal" :icon="primaryIcon" :loading="busy" :disabled="busy" @click="asking = primary">{{ t(`config.vb_cable_${primary}`) }}</MButton>
    </template>
  </MItem>
  <ConfirmDialog
    v-if="asking"
    :show="!!asking"
    dialog-id="vb-cable-confirm"
    :title="t(`config.vb_cable_${asking || 'install'}`)"
    title-icon="cable"
    :close-label="t('_common.close')"
    @close="asking = ''"
  >
    <p>{{ t(`config.vb_cable_${asking || 'install'}_confirm`) }}</p>
    <template #actions>
      <MButton variant="text" @click="asking = ''">{{ t('_common.cancel') }}</MButton>
      <MButton @click="start">{{ t(`config.vb_cable_${asking || 'install'}`) }}</MButton>
    </template>
  </ConfirmDialog>
</template>
