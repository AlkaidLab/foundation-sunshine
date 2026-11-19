import { apiJson, apiPostJson } from '../utils/apiFetch.js'
import { DEFAULT_CATALOG, GAME_IDS, REGION_IDS } from '../utils/sparxie/model.js'

// The Sparxie routes of the host (docs/sparxie-api.md of the host). Every route name and every field the host sends is read here, once; the pages only see the shapes below.
// A failed call is `{ ok: false, error, error_code }` with a 4xx status (apiJson turns it into an Error whose `code` is the error_code).
export const PATHS = Object.freeze({
  status: '/api/sparxie/status',
  games: '/api/sparxie/games',
  identify: '/api/sparxie/identify',
  scan: '/api/sparxie/scan',
  browse: '/api/sparxie/browse',
  open: '/api/sparxie/open',
  artRefresh: '/api/sparxie/art/refresh',
  create: '/api/sparxie/apps',
  profiles: '/api/sparxie/profiles',
  importProfiles: '/api/sparxie/import',
  gameSettings: '/api/sparxie/game-settings',
  notice: '/api/sparxie/warning',
})

export const ABSENT = Object.freeze({ available: false, installed: false, running: false, version: '', exe: '', touchUiSwitch: false, store: Object.freeze({ state: 'unused', profiles: 0 }), warned: true, statement: Object.freeze({ zh: '', en: '' }) })

/** An error the host answered with: `code` is its error_code (sparxie_running, config_unreadable, unrecognised, game_running…) */
export class SparxieError extends Error {
  constructor(code, message, status = 0) { super(message || code); this.code = code; this.status = status }
}
const fail = (e) => { throw e instanceof SparxieError ? e : new SparxieError(e?.code || 'failed', e?.message || 'failed', e?.status || 0) }

const TITLE_ID = /^[a-z][a-z0-9_]{0,31}$/
const str = (v) => (v === null || v === undefined ? '' : String(v))

/** One install as the pages use it, from whatever the host sent */
export function normalizeInstall(raw) {
  if (!raw || typeof raw !== 'object') return null
  const game = TITLE_ID.test(String(raw.game || '')) ? String(raw.game) : ''
  if (!game) return null
  const candidates = Array.isArray(raw.candidates) ? raw.candidates.filter((r) => REGION_IDS.includes(r)) : (REGION_IDS.includes(raw.region) ? [raw.region] : [])
  const region = REGION_IDS.includes(raw.region) && candidates.length <= 1 ? raw.region : (candidates.length === 1 ? candidates[0] : '')
  return {
    id: str(raw.id), game, region, candidates, markers: Array.isArray(raw.markers) ? raw.markers.map(str) : [], sources: Array.isArray(raw.sources) ? raw.sources.map(str).filter(Boolean) : [],
    dir: str(raw.dir), exe: str(raw.exe), version: str(raw.version), biz: str(raw.biz), profile: raw.profile ? str(raw.profile) : '',
    art: raw.art && typeof raw.art === 'object' ? { version: str(raw.art.version), updated: str(raw.art.updated) } : { version: '', updated: '' },
  }
}
/** The version (content hash) of the official poster of each game and region, `{ 'genshin-cn': 'a1b2c3d4' }`: a poster that changes is a new address */
const artVersions = (raw) => Object.fromEntries(Object.entries(raw && typeof raw === 'object' ? raw : {}).filter(([, v]) => v && typeof v.version === 'string' && v.version).map(([id, v]) => [id, v.version]))
const installs = (list) => (Array.isArray(list) ? list.map(normalizeInstall).filter(Boolean) : [])

const FIELD_KEY = /^[A-Za-z][A-Za-z0-9_]{0,31}$/
const FIELD_KINDS = ['choice', 'toggle', 'slider', 'text']
const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null)
/** The items a host describes: [{ key, kind: choice | toggle | slider | text, section?, inputs?: ['touch'], label?, labelEn?, note?, noteEn?, options?, min?, max?, step?, decimals?, scale?: 'log', marks?, unit?, default?, enabledBy? }].
 *  What is not described is not shown; an item that cannot be drawn is dropped. Resolves null when the host sends no list. */
export function normalizeFields(raw) {
  if (!Array.isArray(raw)) return null
  const out = []
  for (const f of raw) {
    if (!f || !FIELD_KEY.test(String(f.key || '')) || !FIELD_KINDS.includes(f.kind)) continue
    const field = { key: f.key, kind: f.kind, section: str(f.section), label: str(f.label), labelEn: str(f.labelEn), unit: str(f.unit), enabledBy: FIELD_KEY.test(String(f.enabledBy || '')) ? f.enabledBy : '' }
    // `inputs`: the inputs an item is for (only touch has a touch interface scale); an item without it is for every input
    if (Array.isArray(f.inputs) && f.inputs.length) field.inputs = f.inputs.map(str).filter(Boolean)
    // `note`: one line the host has about the item (Star Rail's 120 fps), in Chinese and in English; shown under the item
    if (str(f.note)) { field.note = str(f.note); field.noteEn = str(f.noteEn) }
    // the older items send `default`, the described ones `def`
    if (f.default !== undefined && f.default !== null) field.def = f.default
    else if (f.def !== undefined && f.def !== null) field.def = f.def
    if (f.kind === 'choice') {
      const options = (Array.isArray(f.options) ? f.options : []).map((o) => (o && typeof o === 'object' ? { value: o.value, label: str(o.label), labelEn: str(o.labelEn) } : { value: o, label: '', labelEn: '' })).filter((o) => o.value !== undefined && o.value !== null)
      if (!options.length) continue
      field.options = options
    }
    if (f.kind === 'slider') {
      const min = num(f.min)
      const max = num(f.max)
      if (min === null || max === null || min >= max) continue
      Object.assign(field, { min, max, step: num(f.step) ?? 1, decimals: Number.isInteger(f.decimals) ? f.decimals : 0, log: f.scale === 'log', marks: (Array.isArray(f.marks) ? f.marks : []).map(num).filter((m) => m !== null) })
    }
    out.push(field)
  }
  return out.length ? out : null
}

/** The executables per game and region, from the host's `catalog`; a host without it gets the table of this build */
export function normalizeCatalog(raw) {
  // the titles the host sends, in its order; a host that sends none gets the three titles of this build
  const out = {}
  const rows = Array.isArray(raw) ? raw : []
  for (const row of rows) {
    if (!row || !TITLE_ID.test(String(row.game || ''))) continue
    const regions = {}
    for (const r of Array.isArray(row.regions) ? row.regions : []) if (r && REGION_IDS.includes(r.region) && Array.isArray(r.exes)) regions[r.region] = r.exes.map(str)
    out[row.game] = { name: str(row.name), nameEn: str(row.nameEn), aliases: Array.isArray(row.aliases) ? row.aliases.map(str).filter(Boolean) : [], touch: row.touch !== false, exes: Array.isArray(row.exes) ? row.exes.map(str) : [], regions, settings: normalizeFields(row.settings), settingsManaged: row.settingsManaged !== false, sparxieFields: normalizeFields(row.sparxie?.fields) }
  }
  if (!Object.keys(out).length) for (const g of GAME_IDS) out[g] = { name: '', nameEn: '', aliases: [], touch: true, settings: null, sparxieFields: null, exes: [...DEFAULT_CATALOG[g].exes], regions: Object.fromEntries(REGION_IDS.map((r) => [r, [...DEFAULT_CATALOG[g].regions[r]]])) }
  return out
}

export function normalizeStatus(raw) {
  if (!raw || typeof raw !== 'object') return ABSENT
  // not installed: nothing is shown, but the host's statement is kept (it is sent also when the component is absent)
  if (raw.installed === false) return { ...ABSENT, available: true, statement: { zh: str(raw.statement), en: str(raw.statement_en) } }
  const s = raw.store && typeof raw.store === 'object' ? raw.store : {}
  const state = ['data', 'empty', 'unused', 'unreadable'].includes(s.state) ? s.state : 'unused'
  return { available: true, installed: true, running: !!raw.running, version: str(raw.version), exe: str(raw.exe), touchUiSwitch: raw.touchUiSwitch === true, store: { state, profiles: Number(s.profiles) || 0 }, warned: raw.warned !== false, statement: { zh: str(raw.statement), en: str(raw.statement_en) } }
}

export function createClient({ get = apiJson, post = apiPostJson, put = (url, body) => apiPostJson(url, body, { method: 'PUT' }) } = {}) {
  return {
    /** Never throws: a host without the component, or an older host without the route, is "absent" */
    async status() {
      try { return normalizeStatus(await get(PATHS.status)) } catch (e) { if (e?.status === 404 || e?.status === 501 || e?.status === 403) return ABSENT; throw e }
    },
    async games() {
      const raw = await get(PATHS.games).catch(fail)
      return { stored: installs(raw?.stored), detected: installs(raw?.detected), catalog: normalizeCatalog(raw?.catalog), art: artVersions(raw?.art) }
    },
    /** The game and the region of an executable or a folder, kept to the game and region the person chose.
     *  Resolves { install, hasGame }, or { error } with the host's code: unrecognised | wrong_game (allowed: the names that fit) | is_sparxie. */
    async identify(path, { game = '', region = '' } = {}) {
      try {
        const raw = await post(PATHS.identify, { path, game, region })
        const install = normalizeInstall(raw?.install)
        return install ? { install, hasGame: !!raw.has_game } : { error: 'unrecognised' }
      } catch (e) {
        if (['unrecognised', 'wrong_game', 'is_sparxie'].includes(e?.code) || e?.status === 404) return { error: e?.code || 'unrecognised', allowed: Array.isArray(e?.data?.allowed) ? e.data.allowed.map(str) : [] }
        return fail(e)
      }
    },
    /** The host searches a folder level by level from its first level, nearest install first. Resolves { installs, timedOut } or { error, timedOut } (unrecognised | is_sparxie | not_found). */
    async scan(path, { game = '', region = '' } = {}) {
      try {
        const raw = await post(PATHS.scan, { path, game, region })
        return { installs: installs(raw?.installs), timedOut: !!raw?.timed_out }
      } catch (e) {
        if (['unrecognised', 'is_sparxie', 'not_found'].includes(e?.code)) return { error: e.code, timedOut: !!e?.data?.timed_out }
        return fail(e)
      }
    },
    /** One folder (or the drives, for an empty path) as the signed-in person sees it. Read-only. */
    async browse(path = '', { files = false } = {}) {
      const raw = await post(PATHS.browse, { path, files: files ? 'exe' : '' }).catch(fail)
      return { path: str(raw?.path), parent: str(raw?.parent), truncated: !!raw?.truncated, entries: (Array.isArray(raw?.entries) ? raw.entries : []).map((e) => ({ name: str(e.name), path: str(e.path), kind: ['drive', 'dir', 'file'].includes(e.kind) ? e.kind : 'dir', known: !!e.known })) }
    },
    /** Starts Sparxie in the signed-in person's session (never as the host service) */
    async openSparxie() {
      const raw = await post(PATHS.open, {}).catch(fail)
      return { alreadyRunning: !!raw?.already_running }
    },
    /** Changes a game application in place: { name, mode, settings, restore, cover, 'image-path', hero, options } (any of them). Resolves { app, index, wrote }. */
    async updateApp(index, body) {
      const raw = await put(`${PATHS.create}/${index}`, body).catch(fail)
      return { app: raw?.app || null, index, wrote: !!raw?.wrote }
    },
    /** Asks the host to fetch the official art again; resolves when it is done (the art of games that are not installed comes with it) */
    async refreshArt() { const raw = await post(PATHS.artRefresh, {}).catch(() => null); return Number(raw?.updated) || 0 },
    /** Adds the game to Sparxie when it is not there and appends the application to the host's list: { app, index, saved, wrote, state } (state: write_back.state) */
    async createApp({ install, path, region, mode, name, settings, restore, cover, badge, image, hero, options }) {
      const body = { install, path, region, mode, name }
      if (mode === 'touch' && options && typeof options === 'object') body.options = options
      // the Sparxie mark on the poster of a touch application (the host's default: on for the official cover, off for a custom one)
      if (mode === 'touch' && typeof badge === 'boolean') body.badge = badge
      // only the items the person changed; an application that changed none sends {} and follows the game
      body.settings = settings || {}
      if (restore) body.restore = restore
      if (cover === 'custom') { body.cover = 'custom'; if (image) body['image-path'] = image; if (hero) body.hero = hero }
      const raw = await post(PATHS.create, body).catch(fail)
      return { app: raw?.app || null, index: Number.isInteger(raw?.index) ? raw.index : -1, saved: raw?.saved !== false, wrote: !!raw?.wrote, state: str(raw?.write_back?.state) }
    },
    async profiles() {
      const raw = await get(PATHS.profiles).catch(fail)
      return (Array.isArray(raw?.profiles) ? raw.profiles : []).map((p) => ({ id: str(p.id), name: str(p.name), taken: !!p.taken, install: normalizeInstall(p.install) })).filter((p) => p.id && p.install)
    },
    async importProfiles(ids) {
      const raw = await post(PATHS.importProfiles, { ids }).catch(fail)
      return { apps: Array.isArray(raw?.apps) ? raw.apps : [], saved: raw?.saved !== false }
    },
    /** The game's own settings as they are now: { running, values } (values: resolution, display, fps, vsync, renderScale, preset, hdr) */
    async gameSettings(install, { exe = '', region = '' } = {}) {
      const raw = await get(`${PATHS.gameSettings}?install=${encodeURIComponent(install)}${exe ? `&exe=${encodeURIComponent(exe)}` : ''}${region ? `&region=${encodeURIComponent(region)}` : ''}`).catch(fail)
      // `screen`: the size of the display the host streams, when it can read it (Zenless Zone Zero keeps its resolution as a position in the list the game makes from it)
      const screen = raw?.screen && num(raw.screen.width) > 0 && num(raw.screen.height) > 0 ? { width: raw.screen.width, height: raw.screen.height } : null
      // `stream`: the same display while a stream runs, which is the size the next launch covers
      const stream = raw?.stream && num(raw.stream.width) > 0 && num(raw.stream.height) > 0 ? { width: raw.stream.width, height: raw.stream.height, refresh: num(raw.stream.refresh) } : null
      return { running: !!raw?.running, values: raw?.values && typeof raw.values === 'object' ? raw.values : {}, unavailable: Array.isArray(raw?.unavailable) ? raw.unavailable.map(str) : [], screen, ...(stream ? { stream } : {}) }
    },
    /** The person has read the statement about injection and memory writes */
    async acknowledgeNotice() { await post(PATHS.notice, { acknowledged: true }).catch(fail) },
  }
}
