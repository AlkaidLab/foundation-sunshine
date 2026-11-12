<template>
  <div class="m-app m-root doctor-page">
    <MDoctor
      :request="usbRoot.request(request)"
      :repair="usbRoot.repair"
      :can-repair="usbRoot.canRepair"
      :collect="diag.collect"
      :save="diag.save"
      :host-names="diag.hostNames.value"
      :client-names="diag.clientNames.value"
      :locale="String(locale)"
      :close-label="$t('_common.close')"
      :autostart="!!params?.autostart"
      @go="go"
      @open="openUrl"
      @restart="restart"
      @ai="askAi"
    />
    <LogDiagnosisModal
      :show="aiOpen"
      :config="ai.config"
      :isConfigLoading="ai.isConfigLoading.value"
      :isSavingConfig="ai.isSavingConfig.value"
      :isLoading="ai.isLoading.value"
      :result="ai.result.value"
      :error="ai.error.value"
      :localFindings="ai.localFindings.value"
      :localSuggestions="ai.localSuggestions.value"
      @close="aiOpen = false"
      @diagnose="diagnose"
    />
  </div>
</template>

<script setup>
import { inject, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MDoctor } from 'foundation-ui/vue'
import LogDiagnosisModal from '../components/LogDiagnosisModal.vue'
import { useAiDiagnosis } from '../composables/useAiDiagnosis.js'
import { createUsbRootDoctor } from '../composables/useUsbRootDoctor.js'
import { useDiagnostics } from '../composables/useDiagnostics.js'
import { useUpdateStatus } from '../composables/useUpdateStatus.js'
import { hostData } from '../composables/hostData.js'
import { apiFetch } from '../utils/apiFetch.js'
import { openExternalUrl } from '../utils/helpers.js'

// 体检 of the Web UI: the shared page (MDoctor) with this product's way to the host, its diagnostics bundle and 米塔 AI log diagnosis.
defineProps({ params: { type: Object, default: null } })
const { locale, t } = useI18n()
const usbRoot = createUsbRootDoctor({ t })
const shell = inject('webShell', null)

const request = async (path) => {
  // The host's slowest group (encoders) is bounded at 90 s. 110 s lets that answer through
  // and still ends the page when the host never responds.
  try {
    const r = await apiFetch(path, { signal: AbortSignal.timeout(110000) })
    return { ok: r.ok, status: r.status, json: await r.json().catch(() => null) }
  }
  catch (error) {
    const timedOut = error && (error.name === 'TimeoutError' || error.name === 'AbortError')
    return { ok: false, status: 0, json: { error: timedOut ? 'the host did not answer within 110 s' : String(error) } }
  }
}

const hostConfig = ref(null)
const diag = useDiagnostics({ logs: ref(''), config: hostConfig, update: useUpdateStatus() })
onMounted(async () => {
  diag.loadClients()
  try { hostConfig.value = await hostData.refresh('config') } catch { /* the names stay unknown */ }
})

// A fix leads to a page of this product or to the row of a setting
const PAGE_OF = {
  network: () => shell?.openConfig({ tab: 'network' }),
  audio: () => shell?.nav.go('audio-output'),
  display: () => shell?.nav.go('display-output'),
  vdd: () => shell?.nav.go('vdd'),
  components: () => shell?.nav.go('components'),
  logs: () => shell?.nav.go('troubleshooting'),
  config: () => shell?.openConfig({ tab: 'files' }),
  update: () => shell?.nav.go('about'),
}
const go = (target) => {
  const [kind, id] = String(target).split(':')
  if (kind === 'setting') shell?.openSetting(id)
  else PAGE_OF[id]?.()
}
const openUrl = (url) => { openExternalUrl(url).catch(() => {}) }
const restart = async () => { try { await apiFetch('/api/restart', { method: 'POST' }) } catch { /* the connection drops while the host restarts */ } }

// 交给米塔 AI 分析: the log diagnosis of 日志与诊断, with the health check result in front of the log
const ai = useAiDiagnosis()
const aiOpen = ref(false)
const aiContext = ref('')
const askAi = async ({ prompt }) => {
  aiContext.value = prompt
  aiOpen.value = true
  await ai.loadConfig()
  await diagnose()
}
const diagnose = async () => {
  let logs = ''
  try { const r = await apiFetch('/api/logs'); logs = r.ok ? await r.text() : '' } catch { /* the result alone is analysed */ }
  await ai.diagnose(logs || ' ', { context: aiContext.value })
}
</script>
