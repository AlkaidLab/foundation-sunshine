<template>
  <div class="log-actions">
    <MActions :actions="actions" @action="onAction" />
    <span class="dev-trigger" @click="handleDevTap"></span>
  </div>
</template>

<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MActions, useNotify } from 'foundation-ui/vue'
import { apiFetch } from '../utils/apiFetch.js'
import { isSaveCancelled, isSaveCommandUnavailable, saveThroughDialog } from '../utils/saveDialog.js'

// The actions that always belonged to the log: download the log file, the AI diagnosis, copy the configuration, copy the log text.
// The small invisible trigger at the end is the developer-mode easter egg (seven taps in three seconds), kept as it was.
const DEV_TAP_THRESHOLD = 7
const DEV_TAP_TIMEOUT = 3000
const DEV_STORAGE_KEY = 'sunshine_dev_mode'

const props = defineProps({
  copyLogs: { type: Function, required: true },
  copyConfig: { type: Function, required: true },
})
const emit = defineEmits(['openDiagnosis'])
const { t } = useI18n()

const readDevMode = () => {
  try { return localStorage.getItem(DEV_STORAGE_KEY) === '1' } catch { return false }
}
const writeDevMode = (enabled) => {
  try { localStorage.setItem(DEV_STORAGE_KEY, enabled ? '1' : '0') } catch (e) { console.warn('Persist dev mode failed:', e) }
}

const actions = computed(() => [
  { id: 'download', icon: 'download', label: t('troubleshooting.download_logs') },
  { id: 'diagnosis', icon: 'smart_toy', label: t('troubleshooting.ai_diagnosis') },
  { id: 'config', icon: 'content_copy', label: t('troubleshooting.copy_config') },
  { id: 'copy', icon: 'content_paste', label: t('troubleshooting.copy_logs') },
])
const onAction = (action) => {
  if (action.id === 'download') downloadLogs()
  else if (action.id === 'diagnosis') emit('openDiagnosis')
  else if (action.id === 'config') props.copyConfig()
  else props.copyLogs()
}

const downloadLogs = async () => {
  const timestamp = new Date().toISOString().slice(0, 19).replace(/[:.]/g, '-')
  const filename = `sunshine-logs-${timestamp}.txt`

  // Tauri WebView2: fetch into memory then use Rust save_text_file command (dialog + fs write)
  if (window.__TAURI_INTERNALS__) {
    try {
      const response = await apiFetch('/api/logs')
      if (!response.ok) throw new Error(`HTTP ${response.status}`)
      const content = await response.text()
      const saved = await saveThroughDialog((command, args) => window.__TAURI_INTERNALS__.invoke(command, args), 'save_text_file', { content, defaultName: filename, filterName: 'Text Files', extensions: ['txt'] })
      if (saved === null) return
      return
    } catch (e) {
      if (isSaveCancelled(e)) return
      if (!isSaveCommandUnavailable(e)) {
        useNotify().snack(`${t('_common.error')} ${e?.message || e}`)
        return
      }
      console.warn('The control panel has no save dialog, falling back to the browser download:', e)
    }
  }

  // Standard browser: let browser download directly via <a> link
  // Server returns Content-Disposition: attachment, so the browser handles
  // the download natively without buffering the entire file in JS memory.
  try {
    const link = Object.assign(document.createElement('a'), { href: '/api/logs', download: filename })
    document.body.appendChild(link)
    link.click()
    document.body.removeChild(link)
  } catch (e) {
    console.error('Failed to download logs:', e)
  }
}

// Dev mode: 7 taps within 3 seconds
const devMode = ref(readDevMode())
const devTapCount = ref(0)
let devTapTimer = null

const handleDevTap = () => {
  devTapCount.value++
  clearTimeout(devTapTimer)
  devTapTimer = setTimeout(() => { devTapCount.value = 0 }, DEV_TAP_TIMEOUT)

  if (devTapCount.value >= DEV_TAP_THRESHOLD) {
    devTapCount.value = 0
    devMode.value = !devMode.value
    writeDevMode(devMode.value)
    // Dispatch event so other components can react
    window.dispatchEvent(new CustomEvent('sunshine-background-bypass', { detail: { enabled: devMode.value } }))
  }
}
</script>

<style>
.log-actions { display: flex; flex-direction: column; gap: 4px; }
.dev-trigger { display: inline-block; width: 12px; min-height: 24px; cursor: default; user-select: none; opacity: 0; align-self: flex-end; }
</style>
