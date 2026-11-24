import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { decodeLogBytes } from 'foundation-ui/diagnostics'
import { findSessions, parseLog, sessionSummary, sessionClientLabel } from 'foundation-ui/vue'
import { apiFetch, apiJson } from '../utils/apiFetch.js'
import { osFromLog, saveBundle } from '../utils/diagnosticsFiles.js'

// The data of the diagnostics page of the Web UI: the host log, the host configuration and the routes the host already has. Nothing here
// sends anything anywhere; `collect` reads, `save` writes a file on this computer.

export function useDiagnostics({ logs, config, update }) {
  const live = ref(true)
  const liveSession = ref(null)
  const text = computed(() => (logs.value === 'Loading...' ? '' : logs.value))
  const adapters = computed(() => (Array.isArray(config.value?.adapters) ? config.value.adapters.map((a) => a?.name).filter(Boolean) : []))
  const facts = computed(() => ({
    version: config.value?.version || '',
    channel: update.channel.value === 'prerelease' ? 'pre-release' : update.channel.value,
    os: osFromLog(text.value),
    gpu: adapters.value,
    encoder: config.value?.active_encoder || '',
  }))
  const hostNames = computed(() => [config.value?.sunshine_name, window.location.hostname].filter(Boolean))
  const clientNames = ref([])

  // The numbers of a session that is running: the host's runtime route. Past sessions have only what the log says.
  let timer = 0
  const pollLive = async () => {
    try { liveSession.value = (await apiJson('/api/runtime/sessions'))?.sessions?.[0] || null } catch { liveSession.value = null }
  }
  const startPolling = () => { stopPolling(); void pollLive(); timer = setInterval(pollLive, 5000) }
  const stopPolling = () => { clearInterval(timer); timer = 0 }
  onBeforeUnmount(stopPolling)
  watch(live, (on) => (on ? startPolling() : stopPolling()), { immediate: false })

  const statsFor = (summary) => {
    const s = summary.live ? liveSession.value : null
    if (!s) return null
    return [
      sessionClientLabel(s) ? { label: 'Client', value: sessionClientLabel(s) } : null, // the host's placeholder name (「Named Zako」) names no device: the family it derived, else nothing
      s.width && s.height ? { label: 'Resolution', value: `${s.width}×${s.height}` } : null,
      s.fps ? { label: 'FPS', value: String(s.fps) } : null,
      s.bitrate ? { label: 'Bitrate', value: `${Math.round(s.bitrate / 1000)} Mbps` } : null,
    ].filter(Boolean)
  }

  async function loadClients() {
    try { clientNames.value = ((await apiJson('/api/clients/list'))?.named_certs || []).map((c) => c?.name).filter(Boolean) } catch { /* names stay unknown */ }
  }

  async function collect() {
    const out = {}
    const [full, cfg, runtime, clients] = await Promise.allSettled([apiFetch('/api/logs').then(async (r) => (r.ok ? (typeof r.arrayBuffer === 'function' ? decodeLogBytes(new Uint8Array(await r.arrayBuffer())) : await r.text()) : '')), apiJson('/api/config'), apiJson('/api/runtime/sessions'), apiJson('/api/clients/list')])
    const log = full.status === 'fulfilled' ? full.value : ''
    const conf = cfg.status === 'fulfilled' ? cfg.value : config.value
    if (log) out.log = log
    if (conf) {
      out.config = conf
      out.versions = { host: conf.version || '', webui: conf.version || '', browser: navigator.userAgent }
      out.displays = Array.isArray(conf.display_devices) ? conf.display_devices : undefined
      const gpus = Array.isArray(conf.adapters) ? conf.adapters.map((a) => a?.name).filter(Boolean) : []
      if (gpus.length) out.gpu = { adapters: gpus, encoder: conf.active_encoder || '', note: 'The driver version is not reported by the host.' }
      out.os = { platform: conf.platform || '', host: osFromLog(log) }
    }
    if (clients.status === 'fulfilled') out.devices = { clients: (clients.value?.named_certs || []).map((c) => ({ name: c?.name })), gamepad: conf?.gamepad || '' }
    const rows = parseLog(log)
    const sessions = findSessions(rows).map((s) => sessionSummary(rows, s)).map(({ firstError, ...rest }) => ({ ...rest, firstError: firstError?.message || '' }))
    out.sessions = { fromLog: sessions, running: runtime.status === 'fulfilled' ? runtime.value?.sessions || [] : [] }
    return out
  }

  return { live, text, facts, hostNames, clientNames, statsFor, collect, save: saveBundle, loadClients, startPolling }
}
