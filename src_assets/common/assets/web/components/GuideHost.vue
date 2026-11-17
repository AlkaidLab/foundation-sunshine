<template>
  <MOnboardWindow mode="dialog" :label="locale === 'zh' ? '新手引导' : 'Getting started'" @close="$emit('close')">
    <div class="m-app m-root guide-host">
      <MGuideFlow
        v-if="hostProgress"
        :steps="steps"
        :state="flowState"
        :callbacks="flowCallbacks"
        :locale="guideLocale"
        :version="version"
        :mode="mode"
        :restart="restart"
        :host-progress="hostProgress"
        :storage-key="GUIDE_STORAGE_KEY"
        :qr="qrMatrix"
        @close="$emit('close')"
        @finish="(info) => $emit('finish', info)"
      />
    </div>
  </MOnboardWindow>
</template>

<script setup>
import { computed, inject, onMounted, ref } from 'vue'
import QRCode from 'qrcode'
import { MGuideFlow, MOnboardWindow, buildGuideSteps, createHostSaver, hostProgressOf, readProgress, useTelemetryConsent } from 'foundation-ui/vue'
import { GUIDE_STORAGE_KEY, useGuide } from '../composables/useGuide.js'
import { LOCALE_NAMES, SUPPORTED, activeLocale, detectSystemLocale, localeChoice } from '../config/i18n.js'
import { apiJson } from '../utils/apiFetch.js'

// The one first-run flow of the Web UI (the control panel's welcome is the same component): a large window over a full scrim, opened by utils/guideLauncher.js.
// Credentials exist whenever this page is reached, so the account step (the one step only the control panel can have) is left out, and so is the update method (the Web UI keeps no update source).
const props = defineProps({ locale: { type: String, default: 'en' }, version: { type: String, default: '' }, mode: { type: String, default: 'fresh' }, restart: { type: Boolean, default: false }, skip: { type: Array, default: () => [] } })
const emit = defineEmits(['close', 'finish'])
const shell = inject('webShell', null)

// The first screen (the project, the language: 跟随系统 by default) and the usage-count question are shown once: not again when the welcome page already went through them, and the question not at all where it was answered before.
const seen = readProgress(null, GUIDE_STORAGE_KEY).visited
const telemetry = useTelemetryConsent({ api: { getJson: (path) => apiJson(path), postJson: (path, body) => apiJson(path, { method: 'POST', body }) } })
const steps = computed(() => buildGuideSteps({ mode: props.mode, skip: ['account', 'update', ...props.skip, ...(seen.includes('intro') ? ['intro'] : [])] }))
const { state, callbacks } = useGuide()
const guideLocale = computed(() => activeLocale.value || props.locale)
const flowState = computed(() => ({
  ...state,
  locale: { choice: localeChoice.value, resolved: detectSystemLocale(), languages: SUPPORTED.map((code) => ({ code, name: LOCALE_NAMES[code] || code })) },
}))
// the steps done, finished and closed are kept on the host: every device reads the same progress
const hostProgress = ref(null)
const saveGuideProgress = createHostSaver({ getJson: (path) => apiJson(path), postJson: (path, body) => apiJson(path, { method: 'POST', body }) })
// the first screen's one-time notice about error reports links to the switch (关于 › 隐私): the guide closes and the About page opens
const flowCallbacks = { ...callbacks, openPrivacy: () => { emit('close'); shell?.nav.go('about') }, saveGuideProgress }
onMounted(async () => {
  try { hostProgress.value = hostProgressOf(await apiJson('/api/config')) } catch { hostProgress.value = hostProgressOf(null) }
  await telemetry.refresh()
})
// The code is drawn from the matrix of the qrcode package already used for QR pairing; nothing is fetched
const qrMatrix = (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules
</script>

<style>
.guide-host { display: flex; flex-direction: column; flex: 1; min-height: 0; height: 100%; background: transparent; }
</style>
