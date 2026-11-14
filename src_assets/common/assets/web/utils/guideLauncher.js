import { createApp, h } from 'vue'
import { deviceId as guideDeviceId, guideEntry, hostProgressOf, isSetupCompleted, normalizeClients, readProgress, upgradeSkips } from 'foundation-ui/vue/guide.js'
import { hostData } from '../composables/hostData.js'
import { apiJson } from './apiFetch.js'

const STORAGE_KEY = 'foundation.guide.web'
const DISMISSED_KEY = 'foundation.guide.dismissed'
export const GUIDE_SUPPRESS_KEY = 'foundation.guide.suppress'

const session = () => { try { return window.sessionStorage } catch { return null } }

/**
 * Whether the guide opens by itself on this page load. First run only: the host reports setup not completed AND no client is paired
 * AND the guide was not finished, not closed this session, not switched off because the setup wizard was asked for.
 * `clients` is the raw client list answer; an unreadable one never opens it.
 */
export function decideAutoOpen(input = {}) { return !!decideGuideEntry(input) }

/**
 * What the guide does on this page load: { mode: 'fresh' | 'upgrade', skip: [steps already done] } or null. A first install gets the whole flow; a host that exists
 * (setup completed, a client paired) gets the upgrade flow once per version of the Web UI (`version`, the host's own), with the steps that are done left out.
 */
export function decideGuideEntry({ embedded = false, suppressed = false, dismissed = false, config = null, clients = null, progress = null, version = '', host = null, device = '', now = Date.now() } = {}) {
  if (embedded || suppressed || dismissed) return null
  const list = normalizeClients(clients)
  const setupCompleted = config ? isSetupCompleted(config) : undefined
  const pairedCount = list ? list.length : null
  const entry = guideEntry({ progress, version, setupCompleted, pairedCount, configExists: !!config, host: host || hostProgressOf(config), device, now })
  return entry && { ...entry, skip: entry.mode === 'upgrade' ? upgradeSkips({ accountExists: true, setupCompleted, pairedCount }) : [] }
}

let layer = null

/** Opens the guide as a layer over the current page. Calling it twice opens it once. */
export async function openGuide(entry = {}) {
  if (layer) return layer
  const [{ default: GuideHost }, { detectSystemLocale }] = await Promise.all([import('../components/GuideHost.vue'), import('../config/i18n.js')])
  const host = document.createElement('div')
  document.body.appendChild(host)
  let locale = 'en'
  let version = entry.version || ''
  try { const config = await hostData.refresh('config'); locale = config?.locale || detectSystemLocale() || 'en'; version = version || String(config?.version || '') } catch { /* English */ }
  const close = () => { app.unmount(); host.remove(); layer = null }
  const app = createApp({ render: () => h(GuideHost, { locale, mode: entry.mode || 'fresh', restart: entry.restart ?? !entry.mode, version, skip: entry.skip || [], onClose: () => { try { session()?.setItem(DISMISSED_KEY, '1') } catch { /* not remembered */ } close() }, onFinish: (info) => { close(); if (!info?.next) window.location.reload() } }) })
  app.mount(host)
  layer = { close }
  return layer
}

/** Called once per page load: opens the guide when it is a first run (`ignorePaired`: the Web UI, where the setup flag alone says so). Never throws; a host that cannot be read changes nothing. */
export async function maybeAutoOpenGuide({ ignorePaired = false } = {}) {
  try {
    const store = session()
    const embedded = window.isTauri === true && window.parent !== window
    if (embedded || /^\/welcome/.test(window.location.pathname)) return false
    const config = await hostData.refresh('config')
    const version = String(config?.version || '')
    const done = isSetupCompleted(config)
    // a host that is set up has the guide once per new version; without a version of the host it is a first run only
    if (done && !version) return false
    const clients = await apiJson('/api/clients/list')
    // the setup flag is the first-run signal of the Web UI (as the setup wizard it replaced was shown whenever it was not set); the control panel also asks that nobody is paired
    const entry = decideGuideEntry({ embedded, suppressed: store?.getItem(GUIDE_SUPPRESS_KEY) === '1', dismissed: store?.getItem(DISMISSED_KEY) === '1', config, clients: ignorePaired && !done ? { named_certs: [] } : clients, progress: readProgress(null, STORAGE_KEY), version, device: guideDeviceId() })
    if (entry) await openGuide({ ...entry, version })
    return !!entry
  } catch {
    return false
  }
}
