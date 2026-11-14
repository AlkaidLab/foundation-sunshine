import { reactive } from 'vue'
import { checkPortsViaDoctor, isSetupCompleted, normalizeClients, normalizeSessions, setupStateFromConfig } from 'foundation-ui/vue/guide.js'
import { endpointsOfConfig } from 'foundation-ui/settings'
import { apiFetch, apiJson } from '../utils/apiFetch.js'
import { hostData } from './hostData.js'
import { AppService } from '../services/appService.js'
import { markSetupCompleted, saveSetupChoices } from '../services/setupWizardService.js'
import { markLanguageSavedForReload, shouldSkipLanguageStep } from './useSetupWizard.js'
import { postCredentials } from './useWelcome.js'
import { readScan, scanBody, sparxiePlan } from '../utils/hostScan.js'
import { APP_CONSTANTS } from '../utils/constants.js'
import { usePin } from './usePin.js'
import { useVddStatus } from './useVddStatus.js'
import { useReach } from './useReach.js'
import { isTauriEnv, openExternalUrl } from '../utils/helpers.js'
import { translateFallback } from '../utils/localizedMessage.js'
import { saveSettingKey } from 'foundation-ui/settings/save.js'

export const GUIDE_STORAGE_KEY = 'foundation.guide.web'

/**
 * The state and the actions of the guided first run in the Web UI. Every value in `state` comes from the host (`/api/config`,
 * `/api/network/listeners`, `/api/clients/list`, `/api/runtime/sessions`); a source that cannot be read leaves its field empty
 * (`null`) and the guide shows its fallback instead of a made-up value.
 */
export function useGuide() {
  const state = reactive({
    account: { exists: true },
    setup: { completed: false, locale: '', showLanguage: true, adapters: [], displayDevices: [], adapter: '', display: '', prep: 'ensure_only_display', virtual: true },
    host: { platform: '', name: '', addresses: null },
    network: { port: 47989, activePort: 0, upnpActive: null, upnpSaved: null, loaded: false, endpoints: '' },
    webAccess: { scope: 'lan', bind: '', active: null, saved: null },
    reach: useReach().view,
    clients: null,
    sessions: null,
    pair: { name: '' },
    apps: { scanAvailable: true },
  })
  const vdd = useVddStatus()
  const { pairClient, clientError } = usePin()

  async function loadSetup() {
    try {
      const config = await hostData.refresh('config')
      state.host.platform = config.platform || ''
      state.pair.name = config.pair_name || ''
      const done = isSetupCompleted(config)
      // The wizard's rule for the language row, kept: only when no language is saved yet or the browser is Chinese on the default English
      Object.assign(state.setup, setupStateFromConfig(config, { showLanguage: !shouldSkipLanguageStep(config) || (done && ['zh', 'en'].includes(config.locale)) }))
    } catch (error) {
      console.error('Failed to read the host configuration:', error)
    }
  }

  async function saveSetup({ locale, adapter, display, prep, hdr }) {
    if (display === 'ZakoHDR' && state.setup.virtual) {
      await vdd.refreshVddStatus()
      if (!vdd.vddReady.value) {
        try { if (vdd.canManageVdd.value) await vdd.installVdd() } catch { /* the status below decides */ }
        if (!vdd.vddReady.value) return { ok: false, vdd: false }
      }
    }
    const { response } = await saveSetupChoices({ locale, adapter, display, prep: prep ?? state.setup.prep, hdr })
    if (!response.ok) return { ok: false, error: await response.text() }
    state.setup.completed = true
    if (locale) {
      markLanguageSavedForReload()
      setTimeout(() => window.location.reload(), 80)
    }
    return { ok: true }
  }

  async function skipSetup() {
    const { response } = await markSetupCompleted()
    if (response.ok) state.setup.completed = true
  }

  async function refreshAddresses() {
    try {
      const data = await apiJson('/api/network/listeners')
      state.host.addresses = data.status === true && Array.isArray(data.addresses) ? data.addresses : null
    } catch {
      state.host.addresses = null
    }
  }

  /** The listener settings: the base port and UPnP as it runs now and as it is saved (a restart applies the saved one). */
  async function loadNetwork() {
    try {
      const data = await apiJson('/api/network/listeners')
      if (data.status !== true && data.status !== 'true') throw new Error('listeners')
      Object.assign(state.network, { port: Number(data.saved?.port || data.active?.port) || 47989, activePort: Number(data.active?.port) || 0, upnpActive: !!data.active?.upnp, upnpSaved: !!data.saved?.upnp, loaded: true })
      if (Array.isArray(data.addresses)) state.host.addresses = data.addresses
      try {
        const config = await apiJson('/api/config')
        state.network.endpoints = endpointsOfConfig(config).join(', ')
        state.host.name = String(config.sunshine_name || '') // the name clients list (the address step)
      } catch { /* the fields stay as they were */ }
    } catch {
      state.network.loaded = false
    }
  }

  /** Who may open the Web UI and the bind address as saved, and what the host runs now (the choice is applied after a restart). */
  async function loadWebAccess() {
    try {
      const [listeners, config] = await Promise.all([apiJson('/api/network/listeners'), apiJson('/api/config')])
      Object.assign(state.webAccess, { scope: config.origin_web_ui_allowed || 'lan', bind: config.bind_address || '', active: listeners.active || null, saved: listeners.saved || null })
    } catch { /* the step shows the default and the save reports what the host says */ }
  }

  /** origin_web_ui_allowed and/or bind_address in the saved configuration. */
  async function saveWebAccess(extra) {
    try {
      const { response } = await saveSetupChoices({ completed: state.setup.completed, extra })
      if (!response.ok) return { ok: false, error: await response.text() }
      Object.assign(state.webAccess, { scope: extra.origin_web_ui_allowed ?? state.webAccess.scope, bind: extra.bind_address ?? state.webAccess.bind })
      return { ok: true }
    } catch (error) {
      return { ok: false, error: error?.message || '' }
    }
  }

  /** UPnP on or off in the saved configuration (the host applies it after a restart). */
  async function setUpnp(on) {
    try {
      const { response } = await saveSetupChoices({ completed: state.setup.completed, extra: { upnp: on ? 'enabled' : 'disabled' } })
      return response.ok ? { ok: true } : { ok: false, error: await response.text() }
    } catch (error) {
      return { ok: false, error: error?.message || '' }
    }
  }

  /** The base port (`port`) in the saved configuration; the host listens on it after a restart. */
  async function setPort(port) {
    try {
      const { response } = await saveSetupChoices({ completed: state.setup.completed, extra: { port } })
      if (!response.ok) return { ok: false, error: await response.text() }
      state.network.port = Number(port)
      return { ok: true }
    } catch (error) {
      return { ok: false, error: error?.message || '' }
    }
  }

  /** The public endpoints (`external_endpoints`: `host[:port]` entries, comma separated) in the saved configuration; the old single `external_ip` is cleared with the first save. */
  async function setPublicEndpoints(value) {
    try {
      const { response } = await saveSetupChoices({ completed: state.setup.completed, extra: { external_endpoints: value, external_ip: '' } })
      if (!response.ok) return { ok: false, error: await response.text() }
      state.network.endpoints = value
      return { ok: true }
    } catch (error) {
      return { ok: false, error: error?.message || '' }
    }
  }

  async function restartHost() {
    try { const r = await apiFetch('/api/restart', { method: 'POST' }); return { ok: r.ok } } catch { return { ok: true } /* the connection drops while the host restarts */ }
  }

  /** A cheap read of the host, the proof of a restart: the start time of its process (`/api/version`); a host without it answers 404/444, then any answer of the listener route is "it is up". */
  async function probeHost() {
    try {
      const r = await apiFetch('/api/version')
      if (r.ok) return { ok: true, startedAt: String((await r.json().catch(() => null))?.process_started_at || '') }
      if (r.status === 404 || r.status === 444) return { ok: (await apiFetch('/api/network/listeners')).ok, startedAt: '' }
      return { ok: false }
    } catch { return { ok: false } }
  }

  /** The QR window of the host: opened by the pair step and given back when the step goes away. */
  const pairQr = {
    start: async () => { const d = await apiJson('/api/qr-pair', { method: 'POST' }); if (String(d.status) !== 'true') throw new Error(d.error || 'qr'); return d },
    poll: () => apiJson('/api/qr-pair'),
    cancel: () => apiFetch('/api/qr-pair/cancel', { method: 'POST' }),
  }

  async function refreshClients() {
    try {
      state.clients = normalizeClients(await apiJson('/api/clients/list'))
    } catch {
      state.clients = null
    }
  }

  async function refreshSessions() {
    try {
      state.sessions = normalizeSessions(await apiJson('/api/runtime/sessions'))
    } catch {
      state.sessions = null
    }
  }

  async function pair(pin, name) {
    const ok = await pairClient(pin, name)
    return ok ? { ok: true } : { ok: false, error: clientError.value }
  }

  async function restoreApps() {
    try {
      const [apps, platform] = await Promise.all([AppService.getApps(), AppService.getPlatform()])
      const result = AppService.restoreDefaultBuiltInApps(apps, platform, translateFallback)
      if (result.changed === 0) return { ok: true, result: 'none' }
      await AppService.saveApps(result.apps, null)
      return { ok: true, result: 'done' }
    } catch (error) {
      return { ok: false, error: error?.message || '' }
    }
  }

  /** The scan of the game libraries on the host (the same call as the apps page): { ok, games: [{ key, name, platform, ... }] }, or why not. */
  async function scanGames(sources) {
    try {
      const response = await apiFetch('/api/apps/scan', { method: 'POST', body: scanBody({ platforms: sources }) })
      const read = readScan(response.status, await response.json().catch(() => null))
      if (!read.ok) return { ok: false, busy: read.busy, unsupported: read.unsupported, error: read.error }
      return { ok: true, games: read.games.map((game, index) => ({ ...game, key: game.source_path || `${game.name}|${index}`, platform: game['app-type'] || '' })), warnings: read.warnings || [] }
    } catch { return { ok: false } }
  }

  /** Adds the chosen games (or the one typed by hand) to the apps list as plain entries: a name, the program and its folder. */
  async function addGames(games) {
    try {
      const sparxie = games.filter((game) => sparxiePlan(game))
      const plain = games.filter((game) => !sparxiePlan(game))
      for (const game of sparxie) {
        const plan = sparxiePlan(game)
        const path = plan.kind === 'touch' ? '/api/sparxie/import' : '/api/sparxie/apps'
        const body = plan.kind === 'touch' ? { ids: [plan.id] } : { path: plan.path, name: plan.name, mode: 'pc', region: plan.region }
        const response = await apiFetch(path, { method: 'POST', body })
        const data = await response.json().catch(() => ({}))
        if (!response.ok || data?.ok === false) return { ok: false, error: data?.error || '' }
      }
      if (plain.length) {
        const apps = await AppService.getApps()
        const added = plain.map((g) => ({ ...APP_CONSTANTS.DEFAULT_APP, name: g.name, cmd: g.cmd || '', 'working-dir': g['working-dir'] || '', 'image-path': g['image-path'] || '' }))
        await AppService.saveApps([...apps, ...added], null)
      }
      return { ok: true, added: games.length }
    } catch (error) { return { ok: false, error: error?.message || '' } }
  }

  const callbacks = {
    scanGames,
    addGames,
    // the test tools page (the guide ends with an offer to open it)
    openTools: () => { window.location.href = '/#test-tools' },
    openLink: (url) => openExternalUrl(url).catch((error) => console.error('Failed to open URL:', error)),
    copy: (text) => navigator.clipboard.writeText(text),
    loadSetup,
    saveSetup,
    skipSetup,
    refreshAddresses,
    refreshClients,
    refreshSessions,
    loadNetwork,
    loadWebAccess,
    saveWebAccess,
    checkReach: (force, options) => useReach().refresh(force, options),
    setUpnp,
    setPort,
    // the diagnostics page (the address step offers it when the host lists no address)
    openDiagnostics: () => { window.location.href = '/#doctor' },
    // the port check of the address step is the host's own 体检 (network group), the answer the diagnostics page shows, worded in the language of the guide
    checkPorts: async () => {
      let lang = 'en'
      try { lang = /^zh/i.test((await import('../config/i18n.js')).activeLocale.value || '') ? 'zh' : 'en' } catch { /* the page's own language is not at hand: English words */ }
      return checkPortsViaDoctor(async (path) => { const response = await apiFetch(path); return { ok: response.ok, status: response.status, json: await response.json().catch(() => null) } }, lang)
    },
    setPublicEndpoints,
    restartHost,
    probeHost,
    pairQr,
    pair,
    restoreApps,
    // the language of the first screen: shown at once, stored as the choice ('system' or a code); the host's own language (its logs and tray) follows the code that is shown, where the host can be written
    setLocale: async (choice) => {
      const { setLocaleChoice } = await import('../config/i18n.js') // loaded when a language is picked: the translations of the page are not part of the guide's own code
      const code = await setLocaleChoice(choice)
      try { await saveSettingKey({ getJson: (path) => apiJson(path), postJson: (path, body) => apiJson(path, { method: 'POST', body }) }, 'locale', code) } catch { /* not signed in yet: the host keeps its language */ }
    },
    // The apps page takes the request from the address and opens the matching dialog
    openApps: (mode) => { window.location.href = `/apps#guide-${mode}` },
  }
  return { state, callbacks }
}

/** The account step of the welcome page: the host's own password route; the caller leaves for the sign-in afterwards. */
export const createAccountCallback = (translate, { redirectMs = 2000, navigate = (to) => { window.location.href = to } } = {}) => async (form) => {
  const result = await postCredentials({ newUsername: form.username, newPassword: form.password, confirmNewPassword: form.confirm })
  if (!result.ok) return { ok: false, error: `${translate('welcome.error')} ${result.error?.startsWith?.('welcome.') ? translate(result.error) : result.error}` }
  const timer = setTimeout(() => navigate('/'), redirectMs)
  // 立即跳转: the redirect at once (the old page counted 8 s down and had this button)
  return { ok: true, leave: true, now: () => { clearTimeout(timer); navigate('/') } }
}
