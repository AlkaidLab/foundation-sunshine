import { effectScope, ref, watch } from 'vue'
import { usePending, exposePending } from 'foundation-ui/pending'
import { SEARCH_INDEX, constraintText, labelOf, localizedKeyLabel, validateSetting } from 'foundation-ui/settings/index.js'
import { isIpAddress } from '../utils/listenerSettings.js'
import { useConfig } from './useConfig.js'
import { useSharedConfig, setConfigLoadedHook } from './configStore.js'
import { useDualsenseShared, useDualsenseTuning } from './dualsenseShared.js'
import { apiFetch, apiJson } from '../utils/apiFetch.js'
import { AppService } from '../services/appService.js'
import { appsEntries, applyAppEntries, revertAppEntries } from './pendingApps.js'
import { needsRestart } from './restartKeys.js'
import { optionLabelOf } from './optionLabels.js'

// The Web UI's part of the window's unsaved state (foundation-ui/pending): the settings, the gamepad tuning and the application list report their edits
// here, so they stay while the person is on another page, and the one bar saves them. Every source can be saved while its page is not on screen.
const zh = (locale) => String(locale).startsWith('zh')
const INDEX = new Map(SEARCH_INDEX.map((e) => [e.id, e]))
const EXTRA_PAGE = { resolutions: 'tabs.display', fps: 'tabs.display', global_prep_cmd: 'tabs.general' }
// settings whose stored value is the position of an option (the unset value is the first one)
const ENUM = { sleep_mode: ['suspend', 'hibernate', 'away'] }
const EXTRA_LABEL = {
  resolutions: { zh: '分辨率列表', en: 'Resolutions' },
  fps: { zh: '帧率列表', en: 'Frame rates' },
  global_prep_cmd: { zh: '全局准备命令', en: 'Global prep commands' },
}

let started = false
const state = { t: (k) => k, te: () => false, locale: 'en' }

async function restartHost() {
  const response = await apiFetch('/api/restart', { method: 'POST' })
  // the native proxy can answer 503 when the core exits before it replies: that is the restart
  if (response.status !== 503 && !response.ok) throw new Error(`HTTP ${response.status}`)
}

async function activeSessions() {
  try { return (await apiJson('/api/runtime/sessions'))?.total_sessions || 0 } catch { return 0 }
}

export function registerPending({ t, te, locale, embedded = false }) {
  state.t = t
  state.te = te || (() => false)
  state.locale = String(locale)
  if (started) return
  started = true
  const pending = usePending()
  pending.configure({ activeSessions })
  const cfg = useSharedConfig()
  const tr = (key) => state.t(key)

  // ---- the settings ----
  // a value the host would refuse (the base port out of range, a name over 256 characters, a bind address that is not an address) can not be saved
  const invalidOf = (key, value) => {
    const problem = validateSetting(key, value)
    if (problem) return { message: constraintText(state.locale, problem) }
    if (key === 'bind_address' && String(value ?? '').trim() && !isIpAddress(String(value).trim())) return { message: tr('connections.invalid_bind') }
    return undefined
  }
  // the words of a value: the label of the option when the locale has one (sleep_mode_hibernate), on and off for the enabled/disabled pair
  const valueLabel = (key, value) => {
    if (ENUM[key]) { const name = ENUM[key][Number(value ?? 0)]; return name && state.te(`config.${key}_${name}`) ? tr(`config.${key}_${name}`) : undefined }
    if (value === undefined || value === null || value === '') return undefined
    const shown = optionLabelOf(key, value)
    if (shown) return shown
    for (const k of [`config.${key}_${value}`, `config.${key}.${value}`]) if (state.te(k)) return tr(k)
    return undefined
  }
  const configEntry = (e) => {
    const index = INDEX.get(e.key)
    const tab = index?.params?.tab || (e.key in EXTRA_PAGE ? { 'tabs.display': 'display', 'tabs.general': 'general' }[EXTRA_PAGE[e.key]] : 'general')
    const pageKey = index?.path?.[0] || EXTRA_PAGE[e.key] || 'tabs.general'
    return {
      key: e.key,
      label: EXTRA_LABEL[e.key]?.[zh(state.locale) ? 'zh' : 'en'] || labelOf(state.locale, `config.${e.key}`),
      before: e.before,
      after: e.after,
      beforeText: valueLabel(e.key, e.before),
      afterText: valueLabel(e.key, e.after),
      invalid: invalidOf(e.key, e.after),
      page: `config:${tab}`,
      pageLabel: `${tr('shell.stream_settings')} · ${tr(pageKey)}`,
      tab,
      sub: index?.params?.sub,
      focus: index?.params?.focus,
    }
  }
  // the model to save with: the page's own while it is loaded in this document, else one made for the save
  async function modelFor(entries) {
    if (cfg.isLoaded()) return cfg
    const own = useConfig()
    own.initTabs()
    await own.loadConfig()
    own.restoreEntries(entries)
    return own
  }
  const failure = (model) => (model.validationError.value ? tr(model.validationError.value) : model.saveError.value || 'save')
  pending.define('config', {
    page: 'config',
    pageLabel: tr('shell.stream_settings'),
    // only the keys the host reads at start wait for a restart (restartKeys.js)
    restart: (entries) => entries.some((e) => needsRestart(e.key)),
    consequences: (entries) => {
      // a risk is its title (with the severity chip) and one line of what it causes; the value is in the change row
      const risks = (cfg.isLoaded() ? cfg.pendingRisks() : []).map((risk) => ({ id: risk.id, kind: 'risk', severity: risk.severity, title: tr(risk.titleKey), text: tr(risk.descriptionKey) }))
      // some of the changes apply at once: the restart line then names the ones that wait for it
      const waiting = entries.filter((e) => needsRestart(e.key))
      if (waiting.length && waiting.length < entries.length) {
        const names = waiting.map((e) => e.label).join(zh(state.locale) ? '、' : ', ')
        risks.unshift({ id: 'restart', kind: 'restart', text: zh(state.locale) ? `重启后生效：${names}。其余更改保存后立即生效。` : `After the restart: ${names}. The other changes apply when saved.` })
      }
      return risks
    },
    save: async (entries, { restart }) => {
      const model = await modelFor(entries)
      if (!(await model.save())) return { ok: false, error: failure(model) }
      const restarts = restart && entries.some((e) => needsRestart(e.key))
      if (restarts) { try { await restartHost() } catch (error) { return { ok: false, error: error.message } } }
      return { ok: true, restarted: !!restarts }
    },
    revertSaved: async (entries) => {
      const model = await modelFor([])
      model.restoreEntries(entries.map((e) => ({ key: e.key, after: e.before })))
      return (await model.save()) ? { ok: true } : { ok: false, error: failure(model) }
    },
  })
  pending.attach('config', { revert: (key) => cfg.revertKey(key), revertAll: () => { for (const e of cfg.changedEntries()) cfg.revertKey(e.key) } })

  // the shared model is only compared once it holds the host's values and the edits kept in the store are applied to it
  const ready = ref(false)
  setConfigLoadedHook(() => {
    cfg.restoreEntries(pending.entries('config'))
    ready.value = true
    pending.sync('config', cfg.changedEntries().map(configEntry))
  })
  const scope = effectScope(true)
  scope.run(() => {
    watch(() => (ready.value && cfg.isLoaded() ? JSON.stringify(cfg.changedEntries()) : null), (json) => {
      if (json !== null) pending.sync('config', JSON.parse(json).map(configEntry))
    }, { flush: 'post' })
  })
  // edits kept from before a reload of this document: load the model so the risks can be named and a page that opens finds the values
  if (pending.entries('config').length) { cfg.initTabs(); void cfg.loadConfig() }

  // ---- the gamepad tuning ----
  const ds = useDualsenseShared()
  const tuning = useDualsenseTuning(tr, state.locale)
  const dsEntries = () => (ds.snapshot.value?.keys || []).filter((k) => ds.dirty[k.id]).map((k) => ({
    key: String(k.id), label: localizedKeyLabel(k, state.locale), before: k.value, after: ds.edited[k.id], page: 'gamepad', pageLabel: tr('shell.gamepad'),
  }))
  pending.define('dualsense', {
    page: 'gamepad', pageLabel: tr('shell.gamepad'), restart: () => false,
    save: async () => ((await tuning.save()) ? { ok: true } : { ok: false, error: ds.saveMessage.value || 'save' }),
    revertSaved: async (entries) => {
      for (const e of entries) ds.setValue(Number(e.key), e.before)
      return (await tuning.save()) ? { ok: true } : { ok: false, error: ds.saveMessage.value || 'save' }
    },
  })
  const revertDs = (key) => { const k = ds.descriptor(Number(key)); if (k) { ds.edited[k.id] = k.value; delete ds.dirty[k.id] } }
  pending.attach('dualsense', { revert: revertDs, revertAll: () => tuning.discard() })
  scope.run(() => {
    watch(() => JSON.stringify(dsEntries()), (json) => pending.sync('dualsense', JSON.parse(json)), { flush: 'post' })
  })

  // ---- the application list ----
  pending.define('apps', {
    page: 'apps', pageLabel: tr('apps.applications_title'), restart: () => false,
    save: async (entries) => {
      try { await AppService.saveApps(applyAppEntries(await AppService.getApps(), entries), null); return { ok: true } } catch (error) { return { ok: false, error: error.message } }
    },
    revertSaved: async (entries) => {
      try { await AppService.saveApps(revertAppEntries(await AppService.getApps(), entries), null); return { ok: true } } catch (error) { return { ok: false, error: error.message } }
    },
  })

  // the frame of the control panel shows the one bar
  if (embedded) exposePending(pending, {
    post: (message) => window.parent.postMessage(message, '*'),
    listen: (fn) => { const on = (event) => { if (event.source === window.parent) fn(event.data) }; window.addEventListener('message', on); return () => window.removeEventListener('message', on) },
  })
}

export const appsSource = () => usePending()
export { appsEntries }
