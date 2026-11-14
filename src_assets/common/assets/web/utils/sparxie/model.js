// The Sparxie component in the Web UI: what a game, a region and a Sparxie application are, and the pure helpers the pages share. No state and no network here.
//
// A Sparxie application is a normal Sunshine application plus a `sparxie` block (app.sparxie):
//   { game, region, install, profile, mode: 'touch' | 'pc', cover: 'official' | 'custom', restore: 'restore' | 'keep', settings: {…only the items the person changed…}, options: {…Sparxie profile options…} }
// An "install" is one game folder on this PC as the host knows it:
//   { id, game, region, candidates, markers, dir, exe, version, biz, profile }   (profile: the id of its Sparxie profile, null when Sparxie does not have it yet)

import { SPARXIE_MARK_DATA } from './markData.js'
import { normalizeDisplay } from './display.js'

/** The Sparxie mark (暗檬's character art, used with its author's permission; see public/images/SOURCE.md): the picture of every place that marks something as Sparxie. Inlined: the host serves no file path for it. */
export const SPARXIE_MARK = SPARXIE_MARK_DATA

export const GAME_IDS = Object.freeze(['genshin', 'starrail', 'zzz'])
export const REGION_IDS = Object.freeze(['cn', 'bili', 'global'])
export const MODES = Object.freeze(['touch', 'pc'])

/** Where an install came from: the launcher registry, the Bilibili launcher, a store, a folder search or the person's own choice */
export const SOURCE_IDS = Object.freeze(['hoyoplay', 'bili', 'steam', 'epic', 'folder', 'manual'])

/** What each game ships per region (the host sends the same table as `catalog` with the games; this is the answer of a host that does not): Genshin Impact's international client is GenshinImpact.exe, its national and bilibili clients are YuanShen.exe */
export const DEFAULT_CATALOG = Object.freeze({
  genshin: { exes: ['YuanShen.exe', 'GenshinImpact.exe'], regions: { cn: ['YuanShen.exe'], bili: ['YuanShen.exe'], global: ['GenshinImpact.exe'] } },
  starrail: { exes: ['StarRail.exe'], regions: { cn: ['StarRail.exe'], bili: ['StarRail.exe'], global: ['StarRail.exe'] } },
  zzz: { exes: ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'], regions: { cn: ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'], bili: ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'], global: ['ZenlessZoneZero.exe', 'ZenlessZoneZeroBeta.exe'] } },
})

/** The regions a game has, in the order the page shows them */
export const regionsOf = (catalog, game) => REGION_IDS.filter((r) => (catalog?.[game] || DEFAULT_CATALOG[game])?.regions?.[r])
/** The executable names of a game in a region, the ones the file picker and the folder search accept */
export const exesOf = (catalog, game, region) => (catalog?.[game] || DEFAULT_CATALOG[game])?.regions?.[region] || []

/** The badge text of an install's source ('' when the host did not say) */
export const sourceLabel = (install, C) => C?.sources?.[install?.source] || ''

/** Whether an install can be the one of this game in this region: a decided region must be it, an open one must still allow it */
export const fitsRegion = (install, game, region) => !!install && install.game === game && (install.region ? install.region === region : (install.candidates || []).includes(region))

/** The installs the host already knows for a game in a region: the ones Sparxie stores first, then the ones the launcher registry names; the same folder once */
export function installsFor(lists, game, region) {
  const seen = new Set()
  const out = []
  for (const list of lists) {
    for (const i of list || []) {
      if (!fitsRegion(i, game, region)) continue
      const key = `${i.dir}\\${i.exe}`.toLowerCase()
      if (seen.has(key)) continue
      seen.add(key)
      out.push(i)
    }
  }
  return out
}

/** Whether a title answers to what the person typed: its names (Chinese and English), the short names and abbreviations the host sends (星铁, HSR, ZZZ), or its id; case, spaces and punctuation do not matter */
export function matchTitle(id, row, query) {
  const fold = (text) => String(text || '').toLowerCase().replace(/[\s:：·\-_.'’]/g, '')
  const q = fold(query)
  if (!q) return true
  return [id, row?.name, row?.nameEn, ...(row?.aliases || [])].some((v) => fold(v).includes(q))
}

/** The art id of a game when no region is chosen yet: the national poster */
export const gameArtId = (game, region = 'cn') => `${game}-${region}`

/** Per game: the Sparxie options it has (the HoYoverse games have the frame-rate unlock, Zenless Zone Zero has none) */
export const GAME_TRAITS = Object.freeze({
  genshin: { hoyo: true, touchUi: true },
  starrail: { hoyo: true, touchUi: false },
  zzz: { hoyo: false, touchUi: false },
})
/** A title this build does not know has no Sparxie-specific options */
export const traitsOf = (game) => GAME_TRAITS[game] || { hoyo: false, touchUi: false }

export const RESOLUTIONS = Object.freeze(['1280×720', '1600×900', '1920×1080', '2560×1440', '3840×2160'])
/** What each game offers on the settings groups (Collapse Launcher / Starward settings pages; the host takes fullscreen and windowed only: none of the three documents a borderless mode): a value a game does not have is not shown */
export const OFFERS = Object.freeze({
  genshin: { display: ['fullscreen', 'windowed'], fps: [30, 60], vsync: true, renderScale: true, preset: true, hdr: true },
  starrail: { display: ['fullscreen', 'windowed'], fps: [30, 60, 120], vsync: true, renderScale: true, preset: true, hdr: false },
  zzz: { display: ['fullscreen', 'windowed'], fps: [30, 60, 0], vsync: true, renderScale: true, preset: true, hdr: true },
})
/** What a title offers on the settings section when this build has no table for it: the items every game of the reference launchers has */
const GENERIC_OFFERS = Object.freeze({ display: ['fullscreen', 'windowed'], fps: [30, 60], vsync: true, renderScale: true, preset: true, hdr: false })
export const offersOf = (game) => OFFERS[game] || GENERIC_OFFERS
export const GAME_DEFAULTS = Object.freeze({ resolution: '1920×1080', display: 'fullscreen', fps: 60, vsync: false, renderScale: 1, preset: 'high', hdr: false })
/** What a game reports for an item the person never changed in game (the host sends these for Zenless Zone Zero's GENERAL_DATA.bin: 60 fps, V-sync on, render scale 1.0, medium) */
const GAME_OWN_DEFAULTS = Object.freeze({ zzz: { vsync: true, preset: 'medium', renderScale: 1 } })
export const gameDefaults = (game) => ({ ...GAME_DEFAULTS, ...(GAME_OWN_DEFAULTS[game] || {}) })
// the defaults of Sparxie itself (its profile editor starts with the frame-rate unlock on and 120 as the target)
export const SPARXIE_DEFAULTS = Object.freeze({ input: 'touch', fpsUnlock: true, fps: 120, bgLimit: true, bgFps: 10, priority: 'normal', followPreset: false, preset30: 60, preset60: 1000, scaleOverride: false, scale: 400, isDefault: true })

/** The game's own settings an application holds: only the items the person changed (sparse overrides). An item that is not here follows the game's current value and is never written. */
export function heldSettings(raw) {
  const out = {}
  if (raw && typeof raw === 'object') for (const [k, v] of Object.entries(raw)) if (v !== undefined && v !== null) out[k] = v
  return out
}

const BOOL_OPTIONS = ['fpsUnlock', 'bgLimit', 'followPreset', 'scaleOverride', 'isDefault']
const INT_OPTIONS = ['fps', 'bgFps', 'preset30', 'preset60', 'scale']
const BOOL_SETTINGS = ['vsync', 'hdr']
const INT_SETTINGS = ['fps']
const NUM_SETTINGS = ['renderScale']
const typed = (obj, bools, ints, nums) => {
  const out = { ...obj }
  for (const k of bools) if (out[k] === 'true' || out[k] === 'false') out[k] = out[k] === 'true'
  // a number that was stored as text; `fps: 'display'` (the tier that follows the refresh rate) is a word and stays one
  for (const k of ints) if (typeof out[k] === 'string' && /^-?\d+$/.test(out[k])) out[k] = Number(out[k])
  for (const k of nums) if (typeof out[k] === 'string' && /^-?\d+(\.\d+)?$/.test(out[k])) out[k] = Number(out[k])
  return out
}
/** A block as the host stored it with its types back: the generic apps writer (a property tree) saved every value as text ("false", "120"), and a text "false" is a switch that reads on and a segment that matches no option.
 *  The host types its answer too; this keeps the page right with a host that does not. */
export function normalizeBlock(block) {
  const b = { ...(block || {}) }
  const obj = (v) => (v && typeof v === 'object' && !Array.isArray(v) ? v : {})
  if ('options' in b) b.options = typed(obj(b.options), BOOL_OPTIONS, INT_OPTIONS, [])
  if ('settings' in b) b.settings = typed(obj(b.settings), BOOL_SETTINGS, INT_SETTINGS, NUM_SETTINGS)
  if (b.launchTemplateProvisional === 'true' || b.launchTemplateProvisional === 'false') b.launchTemplateProvisional = b.launchTemplateProvisional === 'true'
  if (b.badge === 'true' || b.badge === 'false') b.badge = b.badge === 'true'
  for (const k of ['candidates', 'markers']) if (b[k] === '') b[k] = []
  return b
}

/** JSON with the keys in order: two states are the same when this text is the same */
export function stableJson(value) {
  if (Array.isArray(value)) return `[${value.map(stableJson).join(',')}]`
  if (value && typeof value === 'object') return `{${Object.keys(value).sort().map((k) => `${JSON.stringify(k)}:${stableJson(value[k])}`).join(',')}}`
  return JSON.stringify(value)
}

/** The Sparxie block of an application, or null when it is not a Sparxie application */
export const sparxieOf = (app) => (app && typeof app.sparxie === 'object' && app.sparxie ? app.sparxie : null)

/** Whether the poster of a touch application shows the Sparxie mark: what the block says, else on for the official cover and off for a cover of the person's own */
export const badgeOf = (block) => (typeof block?.badge === 'boolean' ? block.badge : block?.cover !== 'custom')

/** The titles whose Sparxie session can run with keyboard, mouse and gamepad (the PC-interface switch of Genshin and Star Rail, Endfield's PC input); Zenless Zone Zero starts the game directly then */
export const KBM_SPARXIE_GAMES = ['genshin', 'starrail', 'endfield']

/** The input of an application: the stored one, else what its launch was (an application of the first builds that starts the game directly is keyboard, mouse and gamepad) */
export const inputOf = (block) => (block?.options?.input === 'auto' ? 'touch' : block?.options?.input) || (block?.mode === 'pc' ? 'kbm' : 'touch') // the removed automatic input reads as touch

/** Whether the poster of an application carries the Sparxie mark: only an application that starts through Sparxie with the touch input (keyboard, mouse and gamepad have the plain poster) */
export const badgeApplies = (mode, input) => mode === 'touch' && input === 'touch'

/** A block with every field a page reads, filled from the defaults (the host stores only what the person changed) */
export function withDefaults(block) {
  const b = normalizeBlock(block)
  return {
    mode: 'touch', cover: 'official', restore: 'restore',
    ...b,
    // an app that never chose follows the stream
    display: normalizeDisplay(b.display),
    // only the items the person changed; every other item follows the game
    settings: heldSettings(b.settings),
    options: { ...SPARXIE_DEFAULTS, ...(b.options || {}), input: inputOf(b) },
  }
}

/** The chips under the title of a card: Sparxie, the region, the launch mode. `C` is the copy of the page (copy.js) */
export function appChips(app, C) {
  const b = sparxieOf(app)
  if (!b) return []
  // a game app is one type: the Sparxie mark is on the ones that launch through it
  const chips = b.mode === 'pc' ? [] : [{ key: 'spx', icon: 'extension', image: SPARXIE_MARK, tone: 'tertiary', label: 'Sparxie' }]
  if (b.region && C.regions[b.region]) chips.push({ key: 'region', icon: 'public', tone: 'neutral', label: C.regions[b.region] })
  // the chip says the input, never "direct": an application that starts the game itself is keyboard, mouse and gamepad
  const input = inputOf(b)
  chips.push(input === 'kbm' ? { key: 'mode', icon: 'keyboard', tone: 'neutral', label: C.inputKbm } : { key: 'mode', icon: 'touch_app', tone: 'info', label: C.inputTouch })
  return chips
}

/** A long path with the middle cut out, so both ends stay readable on one line */
export function midCut(text, max = 44) {
  if (text.length <= max) return text
  const parts = text.split('\\')
  if (parts.length > 4) return `${parts[0]}\\${parts[1]}\\…\\${parts.slice(-2).join('\\')}`
  return `${text.slice(0, Math.ceil((max - 1) * 0.55))}…${text.slice(text.length - Math.floor((max - 1) * 0.45))}`
}

export const fullPath = (install) => `${install.dir}\\${install.exe}`

/** The region of a choice: the one the person picked, else the one the markers decided; '' while it is open (more than one candidate and no pick) */
export function regionOf(install, pick = '') {
  if (!install) return ''
  if (pick && (install.candidates || []).includes(pick)) return pick
  const c = install.candidates || []
  if (install.region && c.length <= 1) return install.region
  return c.length === 1 ? c[0] : ''
}

/** The name a new application gets: the game and the region; the game alone while the region is open, nothing without a game */
export function defaultName(install, region, C) {
  if (!install) return ''
  const game = C.games[install.game]
  return region ? `${game} · ${C.regions[region]}` : game
}

/** Whether creating the application has to add the game to Sparxie first (its profile does not exist yet) */
export const needsWriteBack = (install) => !!install && !install.profile

/**
 * Whether the create button is available, and why not.
 * store: the status of Sparxie's storage; running: Sparxie is open (it reads its file once, so the host does not write while it runs).
 */
export function createBlock({ install, region, store, mode = 'touch' }) {
  if (!install) return 'no-game'
  if (!region) return 'no-region'
  // 直接启动 does not touch Sparxie's storage: only touch launch needs the game written to it
  if (mode === 'touch' && needsWriteBack(install) && store?.running) return 'running'
  if (mode === 'touch' && needsWriteBack(install) && store?.state === 'unreadable') return 'unreadable'
  return ''
}

/** The Sparxie app block for a new application */
export function newBlock({ install, region, mode }) {
  return {
    game: install.game, region, install: install.id, profile: install.profile || '', mode,
    cover: 'official', restore: 'restore', settings: {}, options: { ...SPARXIE_DEFAULTS }, display: { mode: 'follow' },
  }
}

/** The art of an install from the host's cache, by slot: icon (96×96), card (2:3 cover composed from the official background and logo), banner (16:9, top part kept).
 *  `version` is the content hash of the official image, so a new poster is a new URL and nothing stale stays in the browser.
 *  `badge` asks for the card and the hero with the Sparxie mark drawn on them, the posters of a touch application. */
export function artUrl(slot, install, version = '', badge = false) {
  const id = install?.id || ''
  const query = [version ? `v=${encodeURIComponent(version)}` : '', badge ? 'badge=1' : ''].filter(Boolean).join('&')
  return `/api/sparxie/art/${slot}/${encodeURIComponent(id)}${query ? `?${query}` : ''}`
}

/** Where an install was found: the sources the host names (hoyoplay, steam, epic, bili), Sparxie when its own profile has it, manual when the person chose it */
export function sourcesOf(install, { stored = false, manual = false } = {}) {
  const out = [...(install?.sources || [])]
  if (stored || install?.profile) out.push('sparxie')
  if (manual) out.push('manual')
  return [...new Set(out)]
}

/** The same install seen by two sources is one row with both sources */
export function mergeInstalls(lists) {
  const byKey = new Map()
  for (const { list, stored = false, manual = false } of lists) {
    for (const i of list || []) {
      const key = `${i.game}|${i.dir}\\${i.exe}`.toLowerCase()
      const found = byKey.get(key)
      if (!found) byKey.set(key, { install: i, sources: sourcesOf(i, { stored, manual }) })
      else found.sources = [...new Set([...found.sources, ...sourcesOf(i, { stored, manual })])]
    }
  }
  return [...byKey.values()]
}

/** What a title's Sparxie section holds when the host sends no description of it (a host of an older build): the items of Sparxie's own profile that the title has.
 *  The frame-rate tiers are the host's list, so the items that need one (the target frame rate, the preset mapping) wait for the host. */
export function sparxieFieldsOf(row, game) {
  if (row?.sparxieFields) return row.sparxieFields
  const t = traitsOf(game)
  const f = (key, kind, section, extra = {}) => ({ key, kind, section, label: '', labelEn: '', unit: '', enabledBy: '', ...extra })
  const out = [f('input', 'choice', 'input', { options: [{ value: 'touch', label: '', labelEn: '' }, { value: 'kbm', label: '', labelEn: '' }], def: 'touch' })]
  if (t.hoyo) {
    out.push(f('fpsUnlock', 'toggle', 'fps', { def: true }))
    out.push(f('bgLimit', 'toggle', 'fps', { def: true }))
    out.push(f('bgFps', 'slider', 'fps', { min: 10, max: 1000, step: 1, decimals: 0, log: true, marks: [10, 30, 60], unit: ' FPS', enabledBy: 'bgLimit' }))
    out.push(f('priority', 'choice', 'advanced', { options: ['belowNormal', 'normal', 'aboveNormal', 'high'].map((value) => ({ value, label: '', labelEn: '' })), def: 'normal' }))
  }
  if (t.touchUi) {
    out.push(f('followPreset', 'toggle', 'advanced', { def: false }))
    out.push(f('scaleOverride', 'toggle', 'ui', { def: false, inputs: ['touch'] }))
    out.push(f('scale', 'slider', 'ui', { min: 100, max: 500, step: 5, decimals: 0, unit: '%', marks: [100, 250, 400, 500], enabledBy: 'scaleOverride', inputs: ['touch'] }))
  }
  return out
}

/** A slider with only a few positions (Zenless Zone Zero's render scale: 0.8, 1.0, 1.2) is a choice between them: segments, not a track */
export function fewStepChoice(field) {
  if (field.kind !== 'slider') return field
  const step = field.step || 1
  const count = Math.round((field.max - field.min) / step) + 1
  if (count < 2 || count > 4) return field
  const decimals = field.decimals ?? 0
  const options = Array.from({ length: count }, (_, i) => { const value = Number((field.min + i * step).toFixed(decimals)); const label = `${value.toFixed(decimals)}${field.unit || ''}`; return { value, label, labelEn: label } })
  return { ...field, kind: 'choice', options }
}

/** The items the host cannot read or write for this install (`unavailable` of the game-settings answer: a file of a newer game version that the shipped key does not decode) are not drawn: { shown, hidden } */
export function splitUnavailable(fields, unavailable = []) {
  const off = new Set(unavailable)
  return { shown: fields.filter((f) => !off.has(f.key)), hidden: fields.filter((f) => off.has(f.key)) }
}

/** The game's own settings items of a title: what the host describes, else the items every title of the reference launchers has (`presetKeys`: the graphics presets of the page) */
export function gameFieldsOf(row, game, presetKeys = []) {
  return gameFieldsRaw(row, game, presetKeys).map(fewStepChoice)
}
function gameFieldsRaw(row, game, presetKeys) {
  if (row?.settings) return row.settings
  const o = offersOf(game)
  const f = (key, kind, extra = {}) => ({ key, kind, section: '', label: '', labelEn: '', unit: '', enabledBy: '', ...extra })
  const opt = (value) => ({ value, label: '', labelEn: '' })
  const out = [f('resolution', 'choice', { options: RESOLUTIONS.map(opt) }), f('display', 'choice', { options: o.display.map(opt) }), f('fps', 'choice', { options: o.fps.map(opt) })]
  if (o.preset && presetKeys.length) out.push(f('preset', 'choice', { options: presetKeys.map(opt) }))
  if (o.renderScale) out.push(f('renderScale', 'slider', { min: 0.6, max: 1.4, step: 0.1, decimals: 1, marks: [0.6, 1, 1.4] }))
  if (o.vsync) out.push(f('vsync', 'toggle'))
  if (o.hdr) out.push(f('hdr', 'toggle'))
  return out
}

/** Whether an item shows for an input: the host marks the items that are for some inputs only (`inputs`), every other item shows for all of them */
export const forInput = (field, input) => !field.inputs || field.inputs.includes(input)
