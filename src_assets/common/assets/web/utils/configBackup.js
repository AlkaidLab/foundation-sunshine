import { SEARCH_INDEX, SETTING_META, isChanged, normalizeValue } from 'foundation-ui/settings/index.js'

// The configuration as a file and as a difference from the defaults (the 配置文件 page). The host's GET /api/config answers the keys of sunshine.conf plus facts of the
// running host; an export writes the keys back in the host's own format (`key = value`, one per line), an import reads that format into the unsaved changes of the page, and
// the saved file is only changed by the page's one save. Pure: no DOM, no Vue.

/** Facts the host adds to GET /api/config that are not lines of sunshine.conf (src/confighttp/configuration.cpp getConfig). */
const NOT_CONFIG = new Set(['status', 'platform', 'version', 'display_devices', 'adapters', 'widget_token_configured', 'nvidia_vsync', 'active_encoder', 'active_hevc_mode', 'active_av1_mode', 'active_nvenc_frame_budget', 'usb_forwarding_config_version', 'pair_name', 'error'])
/** A secret is never written to a file a person may pass around. */
const SECRET = /password|salt|token|secret|credential_?key/i
/** The places of files on this computer: they belong to the computer, so an import from another one leaves them alone. */
export const PATH_KEYS = Object.freeze(['file_apps', 'credentials_file', 'file_state', 'log_path', 'pkey', 'cert'])

/** The lines of sunshine.conf in a GET /api/config answer: { key: text } in key order; facts of the host, objects and secrets dropped. */
export function configLines(answer) {
  const out = {}
  for (const key of Object.keys(answer || {}).sort()) {
    const value = answer[key]
    if (NOT_CONFIG.has(key) || SECRET.test(key) || value === null || value === undefined || typeof value === 'object') continue
    out[key] = String(value)
  }
  return out
}

/** The text of a .conf file: a comment line with where and when it came from, then `key = value`. */
export function configText(lines, { host = '', at = new Date() } = {}) {
  const pad = (n) => String(n).padStart(2, '0')
  const when = `${at.getFullYear()}-${pad(at.getMonth() + 1)}-${pad(at.getDate())} ${pad(at.getHours())}:${pad(at.getMinutes())}`
  const head = `# Foundation Sunshine configuration${host ? ` of ${host}` : ''}, exported ${when}`
  return [head, ...Object.entries(lines).map(([k, v]) => `${k} = ${v}`), ''].join('\n')
}

/** The file name of an export: `sunshine-<host>-<yyyymmdd>.conf`. */
export function configFileName(host = '', at = new Date()) {
  const pad = (n) => String(n).padStart(2, '0')
  const safe = String(host || '').trim().replace(/[^\w.-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40)
  return `sunshine${safe ? `-${safe}` : ''}-${at.getFullYear()}${pad(at.getMonth() + 1)}${pad(at.getDate())}.conf`
}

/** The `key = value` lines of a sunshine.conf text (the host's parser: `#` starts a comment line, the first `=` splits). null when nothing in it is a setting line. */
export function parseConfigText(text) {
  const out = {}
  for (const raw of String(text ?? '').split(/\r?\n/)) {
    const line = raw.trim()
    if (!line || line.startsWith('#')) continue
    const at = line.indexOf('=')
    if (at <= 0) continue
    const key = line.slice(0, at).trim()
    if (!/^[a-z0-9_]+$/i.test(key)) continue
    out[key] = line.slice(at + 1).trim()
  }
  return Object.keys(out).length ? out : null
}

/** The lists the page keeps as arrays and writes with their own editors (useConfig serialises them on save). */
const LIST_KEYS = new Set(['fps', 'resolutions', 'global_prep_cmd', 'keybindings'])
/** Whether a key is one setting the page can take as it is (a list, a page of its own or a custom control has its own editor). */
const takesValue = (key, current) => {
  const meta = SETTING_META[key]
  return !!meta && !LIST_KEYS.has(key) && !Array.isArray(current) && !['list', 'page', 'custom'].includes(meta.control)
}

/**
 * What an import would do to the configuration on the page: { changes: [{ key, from, to }], same, paths: [keys], other: [keys] }. `paths` are the file places (left alone),
 * `other` the keys the page cannot take as one value (unknown, a secret, a list or a page of its own).
 */
export function importPlan(lines, config = {}) {
  const plan = { changes: [], same: 0, paths: [], other: [] }
  for (const [key, to] of Object.entries(lines || {})) {
    if (PATH_KEYS.includes(key)) { plan.paths.push(key); continue }
    if (SECRET.test(key) || NOT_CONFIG.has(key) || !takesValue(key, config[key])) { plan.other.push(key); continue }
    const from = config[key] === undefined || config[key] === null ? '' : String(config[key])
    if (normalizeValue(from) === normalizeValue(to)) plan.same += 1
    else plan.changes.push({ key, from, to })
  }
  return plan
}

/** The settings whose value is not the default, in the order of the search index (the order of the pages): [{ key, value, def, place: { tab, sub?, focus } | null, title }]. */
export function differences(config = {}) {
  const order = new Map(SEARCH_INDEX.map((e, i) => [e.id, i]))
  return Object.keys(SETTING_META)
    .filter((key) => isChanged(key, config[key]))
    .sort((a, b) => (order.get(a) ?? 1e6) - (order.get(b) ?? 1e6) || a.localeCompare(b))
    .map((key) => {
      const hit = SEARCH_INDEX.find((e) => e.id === key)
      const def = SETTING_META[key].def
      return { key, value: String(config[key]), def: def === null || def === undefined ? '' : String(def), place: hit ? { tab: hit.params.tab, ...(hit.params.sub ? { sub: hit.params.sub } : {}), focus: key } : null, title: hit ? hit.title : `config.${key}` }
    })
}
