<template>
  <div class="m-app m-root troubleshooting-page">
    <MDiagnostics
      :title="diagnosticsTexts(String(locale)).title"
      :text="diag.text.value"
      v-model:live="diag.live.value"
      :facts="diag.facts.value"
      :collect="diag.collect"
      :save="diag.save"
      :stats-for="diag.statsFor"
      :host-names="diag.hostNames.value"
      :client-names="diag.clientNames.value"
      :locale="String(locale)"
      :close-label="$t('_common.close')"
      :commands="commands"
      :notes="notes"
      :disabled="disabled"
      :labels="labels"
      :ai="aiSheet"
      :log-loading="logs === 'Loading...'"
      @open="openUrl"
      @go="goPlace"
    >
      <template #actions>
        <HelpPromos />
        <span class="dev-trigger" @click="handleDevTap" />
      </template>
    </MDiagnostics>

    <ConfirmDialog
      :show="showBoomConfirmModal"
      dialog-id="boom-confirm"
      :title="$t('troubleshooting.confirm_boom')"
      title-icon="warning"
      tone="danger"
      :close-label="$t('_common.close')"
      @close="closeBoomModal"
    >
      <p>{{ $t('troubleshooting.confirm_boom_desc') }}</p>
      <template #actions>
        <MButton variant="text" @click="closeBoomModal">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" icon="warning" @click="confirmBoom">{{ $t('troubleshooting.boom_sunshine') }}</MButton>
      </template>
    </ConfirmDialog>

    <ConfirmDialog
      :show="showLogoutConfirmModal"
      dialog-id="logout-confirm"
      :title="$t('troubleshooting.confirm_logout')"
      title-icon="logout"
      tone="danger"
      :close-label="$t('_common.close')"
      @close="closeLogoutModal"
    >
      <p>{{ $t('troubleshooting.confirm_logout_desc') }}</p>
      <template #actions>
        <MButton variant="text" @click="closeLogoutModal">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" icon="logout" @click="confirmLogout">{{ $t('troubleshooting.logout') }}</MButton>
      </template>
    </ConfirmDialog>

    <ConfirmDialog
      :show="showLocalhostLogoutModal"
      dialog-id="localhost-logout-notice"
      :title="$t('troubleshooting.logout')"
      title-icon="info"
      :close-label="$t('_common.close')"
      @close="closeLocalhostLogoutModal"
    >
      <p>{{ $t('troubleshooting.logout_localhost_tip') }}</p>
      <template #actions>
        <MButton @click="closeLocalhostLogoutModal">{{ $t('_common.close') }}</MButton>
      </template>
    </ConfirmDialog>
    <MRemoteWindow v-model="remoteOpen" :locale="String(locale)" :state="guideData.state" :callbacks="guideData.callbacks" />
  </div>
</template>

<script setup>
import { computed, inject, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MDiagnostics, MButton, MRemoteWindow, coachTexts, guideTexts, homeTexts, remoteTexts, diagnosticsTexts, useGuideCard } from 'foundation-ui/vue'
import { useGuide } from '../composables/useGuide.js'
import HelpPromos from '../components/common/HelpPromos.vue'
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import { useTroubleshooting } from '../composables/useTroubleshooting.js'
import { useLogout } from '../composables/useLogout.js'
import { useAiDiagnosis } from '../composables/useAiDiagnosis.js'
import { useDiagnostics } from '../composables/useDiagnostics.js'
import { useUpdateStatus } from '../composables/useUpdateStatus.js'
import { hostData } from '../composables/hostData.js'
import { openExternalUrl } from '../utils/helpers.js'
import { apiFetch } from '../utils/apiFetch.js'
import { saveThroughDialog } from '../utils/saveDialog.js'

// 诊断与日志. The same page the control panel shows. A row exists when this product can do it: sign-out is a browser session, the panel log is not here.
const { t, locale } = useI18n()
const guideText = computed(() => guideTexts(locale.value))
const remoteText = computed(() => remoteTexts(String(locale.value)))
const remoteOpen = ref(false)
const coachText = computed(() => coachTexts(String(locale.value)))
const startCoach = () => window.dispatchEvent(new CustomEvent('foundation-coach-start'))
const guideData = useGuide()
const startText = computed(() => homeTexts(locale.value))
const startCard = useGuideCard('web-home', () => [])
const openGuide = async () => (await import('../utils/guideLauncher.js')).openGuide()
const { logout } = useLogout()

const {
  config: aiConfig, isConfigLoading: aiConfigLoading, isSavingConfig: aiSavingConfig, isLoading: aiLoading,
  result: aiResult, error: aiError, localFindings: aiLocalFindings, localSuggestions: aiLocalSuggestions,
  diagnose: aiDiagnose, loadConfig: loadAiConfig,
} = useAiDiagnosis()
const showDiagnosisModal = ref(false)

const {
  platform, closeAppPressed, closeAppStatus, restartPressed, boomPressed,
  resetDisplayDevicePressed, resetDisplayDeviceStatus, logs, actualLogs, refreshLogs,
  closeApp, restart, boom, resetDisplayDevicePersistence, copyLogs, copyConfig, reopenSetupWizard,
  loadPlatform, startLogRefresh, stopLogRefresh,
} = useTroubleshooting()

const showBoomConfirmModal = ref(false)
const showLogoutConfirmModal = ref(false)
const showLocalhostLogoutModal = ref(false)
const downloadError = ref(null)
const closeBoomModal = () => { showBoomConfirmModal.value = false }
const confirmBoom = () => { closeBoomModal(); boom() }
const closeLogoutModal = () => { showLogoutConfirmModal.value = false }
const closeLocalhostLogoutModal = () => { showLocalhostLogoutModal.value = false }
const confirmLogout = () => {
  closeLogoutModal()
  showLocalhostLogoutModal.value = false
  stopLogRefresh()
  logout({ onLocalhost: () => { showLocalhostLogoutModal.value = true } })
}
const handleCopyConfig = () => copyConfig(t)
const openDiagnosis = async () => { showDiagnosisModal.value = true; await loadAiConfig() }

const DEV_TAP_THRESHOLD = 7
const DEV_TAP_TIMEOUT = 3000
const DEV_STORAGE_KEY = 'sunshine_dev_mode'
const devTapCount = ref(0)
let devTapTimer = null
const handleDevTap = () => {
  devTapCount.value++
  clearTimeout(devTapTimer)
  devTapTimer = setTimeout(() => { devTapCount.value = 0 }, DEV_TAP_TIMEOUT)
  if (devTapCount.value >= DEV_TAP_THRESHOLD) {
    devTapCount.value = 0
    let enabled = false
    try { enabled = localStorage.getItem(DEV_STORAGE_KEY) !== '1'; localStorage.setItem(DEV_STORAGE_KEY, enabled ? '1' : '0') } catch { /* the preference stays as it was */ }
    window.dispatchEvent(new CustomEvent('sunshine-background-bypass', { detail: { enabled } }))
  }
}

async function downloadLogs() {
  downloadError.value = null
  const filename = 'sunshine-logs-' + new Date().toISOString().slice(0, 19).replace(/[:.]/g, '-') + '.txt'
  if (window.__TAURI_INTERNALS__) {
    try {
      const response = await apiFetch('/api/logs')
      if (!response.ok) throw new Error('HTTP ' + response.status)
      const content = await response.text()
      const saved = await saveThroughDialog((command, args) => window.__TAURI_INTERNALS__.invoke(command, args), 'save_text_file', { content, defaultName: filename, filterName: 'Text Files', extensions: ['txt'] })
      if (saved === null) return
      return
    } catch (e) {
      downloadError.value = { text: `${t('_common.error')} ${e?.message || e}`, tone: 'error' }
      return
    }
  }
  try {
    const link = Object.assign(document.createElement('a'), { href: '/api/logs', download: filename })
    document.body.appendChild(link); link.click(); document.body.removeChild(link)
  } catch (e) { downloadError.value = { text: `${t('_common.error')} ${e?.message || e}`, tone: 'error' } }
}

const hostConfig = ref(null)
const diag = useDiagnostics({ logs, config: hostConfig, update: useUpdateStatus() })
watch(diag.live, (on) => { if (on) { refreshLogs(); startLogRefresh() } else stopLogRefresh() })
const shell = inject('webShell', null)
const openUrl = (url) => { openExternalUrl(url).catch((error) => console.error('Failed to open external URL:', error)) }
const PLACES = { encoder: { tab: 'encoders' }, network: { tab: 'network' }, audio: { tab: 'audio', sub: 'sound' }, display: { tab: 'display', sub: 'output' } }
const goPlace = (place) => { if (PLACES[place] && shell?.nav) shell.nav.go('config', PLACES[place]) }

const noteText = (ok, yes, no) => (ok === true ? yes : ok === false ? { text: no, tone: 'error' } : '')
const notes = computed(() => ({
  downloadLogs: downloadError.value || '',
  restart: restartPressed.value ? t('troubleshooting.restart_sunshine_success') : '',
  resetDisplay: noteText(resetDisplayDeviceStatus.value, t('troubleshooting.reset_display_device_success_windows'), t('troubleshooting.reset_display_device_error_windows')),
  forceClose: noteText(closeAppStatus.value, t('troubleshooting.force_close_success'), t('troubleshooting.force_close_error')),
  boom: boomPressed.value ? t('troubleshooting.boom_sunshine_success') : '',
}))
const disabled = computed(() => ({
  restart: restartPressed.value,
  resetDisplay: resetDisplayDevicePressed.value,
  forceClose: closeAppPressed.value,
  boom: boomPressed.value,
}))
const labels = computed(() => ({
  guide: { title: guideText.value.guide, text: guideText.value.helpRow },
  coach: { title: coachText.value.rerun },
  remote: { title: remoteText.value.openText },
  startCard: { title: startText.value.guideTitle, text: startText.value.guideSub },
}))
const commands = computed(() => ({
  restart,
  resetDisplay: platform.value === 'windows' ? resetDisplayDevicePersistence : undefined,
  ai: openDiagnosis,
  diagnose: () => aiDiagnose(actualLogs.value),
  downloadLogs,
  copyLogs,
  copyConfig: handleCopyConfig,
  guide: openGuide,
  coach: startCoach,
  remote: () => { remoteOpen.value = true },
  startCard: () => startCard.reopen(),
  wizard: () => reopenSetupWizard(t),
  forceClose: closeApp,
  boom: () => { showBoomConfirmModal.value = true },
  logout: () => { showLogoutConfirmModal.value = true },
}))
const aiSheet = computed(() => ({
  show: showDiagnosisModal.value,
  config: aiConfig,
  isConfigLoading: aiConfigLoading.value,
  isSavingConfig: aiSavingConfig.value,
  isLoading: aiLoading.value,
  result: aiResult.value,
  error: aiError.value,
  localFindings: aiLocalFindings.value,
  localSuggestions: aiLocalSuggestions.value,
  onClose: () => { showDiagnosisModal.value = false },
}))

onMounted(async () => {
  await Promise.all([loadPlatform(), refreshLogs()])
  startLogRefresh()
  diag.startPolling()
  diag.loadClients()
  try { hostConfig.value = await hostData.refresh('config') } catch { /* the facts stay empty */ }
})
</script>

<style>
.dev-trigger { display: inline-block; width: 12px; min-height: 24px; cursor: default; user-select: none; opacity: 0; }
</style>
