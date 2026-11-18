<script setup>
import { computed } from 'vue'
import { MBanner, MButton } from 'foundation-ui/vue'
import { noDefaultOutputNotice } from '../../utils/audioDevices.js'

// The computer has no default playback device (nothing plugged in or enabled: a server, a cloud machine, a VM). The stream goes on
// without sound and the host starts to capture by itself when a device appears, so this says why and what to do, and takes the person
// to the two rows that add a virtual speaker.
const props = defineProps({
  /** { supported, defaultOutput } of utils/audioDevices.js normalizeAudioDevices */
  state: { type: Object, required: true },
  loaded: { type: Boolean, default: false },
  failed: { type: Boolean, default: false },
})
const notice = computed(() => noDefaultOutputNotice(props.state, props.loaded && !props.failed))
const goTo = (selector) => {
  const row = document.querySelector(selector)
  if (!row) return
  row.scrollIntoView({ behavior: 'smooth', block: 'center' })
  row.focus?.({ preventScroll: true })
}
</script>

<template>
  <MBanner v-if="notice" data-no-default-output :data-reason="notice.reason" tone="warn" icon="headphones">
    <strong>{{ $t('config.audio_no_default_title') }}</strong>
    <div>{{ $t(`config.audio_no_default_${notice.reason}`) }}</div>
    <div>{{ $t('config.audio_no_default_how') }}</div>
    <div v-if="notice.hostWaiting" role="status">{{ $t('config.audio_no_default_waiting') }}</div>
    <div>
      <MButton size="sm" variant="text" icon="cable" @click="goTo('[data-vb-cable]')">VB-CABLE</MButton>
      <MButton size="sm" variant="text" icon="speaker" @click="goTo('[data-steam-driver]')">Steam Streaming Speakers</MButton>
    </div>
  </MBanner>
</template>
