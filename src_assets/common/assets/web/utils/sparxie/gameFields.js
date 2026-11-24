// How the game's own settings are laid out on the editor page: a pure description (no Vue, no network) of what the host sends for a title, so the page and its tests agree.
// The host describes the items (kind, section, options, range); this file decides which of them are drawn, in which group, as which control, and what the numbers of a title mean in words.

/** The order of the groups, and the key of their title in the copy */
export const SECTIONS = Object.freeze(['display', 'quality', 'audio', 'language', 'launch'])
export const SECTION_TITLES = Object.freeze({ display: 'secDisplay', quality: 'secQuality', audio: 'secAudio', language: 'secLanguage', launch: 'secLaunch' })

/** The titles of miHoYo: the host keeps the items of Collapse Launcher for them */
export const HOYO_GAMES = Object.freeze(['genshin', 'starrail', 'zzz', 'hi3'])

// Items the host describes but the page does not draw, with the reason (the reason is for the next person who reads this file):
//   resizable           「可调整大小的窗口」 is not a launch argument in Collapse: its launcher keeps a hook that rewrites the style of the game's window every 250 ms while the game runs
//                       (Hi3Helper.EncTool ResizableWindowHook: border and sizing frame on, put back whenever the game takes them away). The host stores the flag but has no such hook,
//                       so a switch would do nothing. It comes back with a window watcher in the host.
//   launchWidth, launchHeight, launchFullscreen, popupWindow
//                       the older launch items of the first community round; the miHoYo titles have the same things as 自定义分辨率, 显示模式 and 无边框窗口.
//   graphicsApi         Collapse only passes it to the game for Honkai Impact 3rd and Zenless Zone Zero; Genshin and Star Rail take 图形 API from `dxVersion`.
//   dxVersion           the same item for the two titles where graphicsApi is the real one.
//   display items     the size and the window mode are the app's 显示 choice (跟随串流 or 固定), which the host applies at every launch: a second control for the same thing would fight it.
//                       Exclusive full screen and the custom size are launch arguments of the same choice.
const ALWAYS_HIDDEN = new Set(['resizable'])
/** The titles whose size and window mode are the app's 显示 choice: the miHoYo titles and the two Unreal titles through their own settings, Endfield through the size of its Sparxie session */
export const DISPLAY_GAMES = Object.freeze(['genshin', 'starrail', 'zzz', 'hi3', 'wuwa', 'nte', 'endfield'])
const DISPLAY_OWNED = new Set(['resolution', 'display', 'resolutionIndex', 'exclusiveFullscreen', 'borderless', 'customResolution', 'customWidth', 'customHeight', 'launchWidth', 'launchHeight', 'launchFullscreen', 'popupWindow'])
const HOYO_HIDDEN = new Set(['launchWidth', 'launchHeight', 'launchFullscreen', 'popupWindow'])
const HIDDEN_FOR = Object.freeze({ genshin: ['graphicsApi'], starrail: ['graphicsApi'], zzz: ['dxVersion'], hi3: ['dxVersion'] })
export function isHidden(game, key) {
  if (ALWAYS_HIDDEN.has(key)) return true
  if (DISPLAY_GAMES.includes(game) && DISPLAY_OWNED.has(key)) return true
  if (!HOYO_GAMES.includes(game)) return false
  return HOYO_HIDDEN.has(key) || (HIDDEN_FOR[game] || []).includes(key)
}

// ---- custom launch arguments ----
// The host accepts tokens of letters, digits and _ . / : = + - (a token may sit in double quotes), separated by spaces, tabs or line breaks; anything else would reach a shell.
export const ARGS_ALLOWED = /[A-Za-z0-9_./:=+-]/
/** What is wrong with the text, or null: { line, ch, kind: 'char' | 'quote' } (line from 1). The rule is the host's (quote_args in hoyo_settings.cpp): the page says why before the host refuses. */
export function customArgsProblem(text) {
  const lines = String(text ?? '').split(/\r\n|\r|\n/)
  for (let n = 0; n < lines.length; n += 1) {
    const raw = lines[n]
    let i = 0
    while (i < raw.length) {
      while (i < raw.length && (raw[i] === ' ' || raw[i] === '\t')) i += 1
      if (i >= raw.length) break
      if (raw[i] === '"') {
        i += 1
        while (i < raw.length && raw[i] !== '"') {
          if (!ARGS_ALLOWED.test(raw[i])) return { line: n + 1, ch: raw[i], kind: 'char' }
          i += 1
        }
        if (i >= raw.length) return { line: n + 1, ch: '"', kind: 'quote' }
        i += 1
      } else {
        while (i < raw.length && raw[i] !== ' ' && raw[i] !== '\t') {
          if (!ARGS_ALLOWED.test(raw[i])) return { line: n + 1, ch: raw[i], kind: 'char' }
          i += 1
        }
      }
    }
  }
  return null
}

// ---- Zenless Zone Zero: the resolution is a position in the list the game makes from the screen (Collapse Launcher ZenlessGameSettingsPage) ----
// 0 is the screen's own (native) resolution; then one full-screen entry per height of the table that fits the screen, at the screen's aspect ratio; then the windowed 16:9 entries
// (and the screen's own size first, when it is 21:9). The game itself stores -1 until it has been changed, which reads as 0.
const ZZZ_HEIGHTS = Object.freeze([4320, 2880, 2160, 1440, 1280, 1080, 900, 720])
export function zzzResolutions(screen) {
  const width = Number(screen?.width)
  const height = Number(screen?.height)
  if (!(width > 0 && height > 0)) return []
  const ratio = width / height
  const heights = ZZZ_HEIGHTS.filter((h) => h <= height)
  const out = [{ index: 0, mode: 'fullscreen', width, height, native: true }]
  for (const h of heights) out.push({ index: out.length, mode: 'fullscreen', width: Math.round(h * ratio), height: h })
  if (Math.abs(ratio - 21 / 9) < 0.01) out.push({ index: out.length, mode: 'windowed', width, height })
  for (const h of heights) out.push({ index: out.length, mode: 'windowed', width: Math.round(h * (16 / 9)), height: h })
  return out
}
/** The item as a choice between those named resolutions; null when the host cannot tell the screen (a position means nothing then) */
export function zzzResolutionField(field, screen, C) {
  const list = zzzResolutions(screen)
  if (!list.length) return null
  const options = list.map((r) => {
    const label = `${r.mode === 'windowed' ? C.display.windowed : C.display.fullscreen} · ${r.width}×${r.height}`
    return { value: r.index, label: r.native ? `${label} (${C.native})` : label, labelEn: r.native ? `${label} (${C.native})` : label }
  })
  return { ...field, kind: 'choice', options, def: 0, normalize: (v) => (typeof v === 'number' && v >= 0 && v < options.length ? v : 0) }
}

// ---- which control draws an item ----
// the width of a label in letters: a Chinese character is two
const textWidth = (text) => [...String(text)].reduce((n, c) => n + (c.charCodeAt(0) > 0x2e80 ? 2 : 1), 0)
/** The one rule for how a choice is drawn, for every title and every page (the game's own settings, Sparxie's own items, the 显示 choice): up to four answers whose names are short
 *  (as many of the longest name as there are answers fit in 34 letters, a Chinese character counts two) are segments, one press away; anything longer is a menu, and a long list searches. The same kind of item with the same number of answers
 *  therefore gets the same control in every game; only the width of the words in a language can move an item from one to the other. */
export const SEGMENT_MAX_OPTIONS = 4
export const SEGMENT_MAX_WIDTH = 34
// the segments of a row are of one width (the longest name sets it), so the row needs as many of the longest name as there are answers
export const segmentsFit = (labels) => labels.length >= 1 && labels.length <= SEGMENT_MAX_OPTIONS && labels.length * Math.max(...labels.map(textWidth)) <= SEGMENT_MAX_WIDTH
/** What a row of segments needs in pixels (a letter 7 px, 24 px of padding to a segment): more than a cell of the settings grid (208 px) and the item takes the whole row of the grid */
export const segmentsNeed = (labels) => labels.length * (Math.max(0, ...labels.map(textWidth)) * 7 + 24)
export const GRID_CELL = 200
/** 'switch' | 'segments' | 'select' | 'slider' | 'number' | 'args' | 'text' */
export function controlOf(field, zh = true, labelOf = null) {
  if (field.kind === 'toggle') return 'switch'
  if (field.kind === 'text') return field.key === 'customArgs' ? 'args' : 'text'
  if (field.kind === 'slider') {
    const steps = (field.max - field.min) / (field.step || 1)
    // a pixel size or a number with hundreds of positions is typed, not dragged
    return steps > 200 ? 'number' : 'slider'
  }
  // the names as the page shows them (the host's label, else the page's words for the value), so the choice of control is made on what a person reads
  const name = (o) => (labelOf ? labelOf(field, o) : '') || (zh ? o.label || o.labelEn : o.labelEn || o.label) || String(o.value)
  return segmentsFit(field.options.map(name)) ? 'segments' : 'select'
}

// the items shown first within their group: what a person sets before anything else
const FIRST = Object.freeze({ display: ['resolution', 'resolutionIndex', 'display', 'fps'], quality: ['preset', 'renderScale'] })
const RANK = { select: 0, segments: 0, slider: 1, switch: 2, number: 3, text: 3, args: 4 }
// the graphics group opens with its sliders (render scale), the others with their menus
const RANK_QUALITY = { ...RANK, slider: -1 }

/**
 * The groups of the page: [{ section, cells: [{ field, control }] }] in the order of SECTIONS.
 * Within a group the items with a menu or segments come first, then sliders, then switches, then typed values (host order kept inside each kind, `FIRST` before the rest);
 * an item that is on for another item (the custom width for 自定义分辨率) stays after the switch it depends on.
 */
export function layoutGroups(fields, { game = '', zh = true, screen = null, C = null, labelOf = null } = {}) {
  const by = new Map()
  for (const f0 of fields) {
    if (isHidden(game, f0.key)) continue
    let f = f0
    if (f.key === 'resolutionIndex' && game === 'zzz') {
      f = C ? zzzResolutionField(f, screen, C) : null
      if (!f) continue
    }
    const section = SECTIONS.includes(f.section) ? f.section : 'quality'
    if (!by.has(section)) by.set(section, [])
    by.get(section).push({ field: f, control: controlOf(f, zh, labelOf) })
  }
  const groups = []
  for (const section of SECTIONS) {
    const cells = by.get(section)
    if (!cells) continue
    const first = FIRST[section] || []
    const at = (cell) => {
      const k = first.indexOf(cell.field.key)
      return [(section === 'quality' ? RANK_QUALITY : RANK)[cell.control] ?? 5, k < 0 ? 99 : k]
    }
    const sorted = cells.map((c, i) => ({ c, i })).sort((a, b) => {
      const x = at(a.c)
      const y = at(b.c)
      return x[0] - y[0] || x[1] - y[1] || a.i - b.i
    }).map(({ c }) => c)
    groups.push({ section, cells: sorted })
  }
  return groups
}

/** The groups whose items the host cannot read for this install are drawn without them; the page says which (see model.splitUnavailable) */
export const withoutKeys = (groups, keys) => groups.map((g) => ({ ...g, cells: g.cells.filter((c) => !keys.has(c.field.key)) })).filter((g) => g.cells.length)

// ---- what stops a save: the host refuses these values, so the page says so first ----
/** A number outside the range of its item (or not a number) */
export const outOfRange = (field, value) => {
  const n = Number(value)
  return !Number.isFinite(n) || n < field.min || n > field.max
}
/** The first value of a preset the host would refuse: { key, kind: 'range' | 'args' } or null. `valueOf(key, field)` is the value the page holds (stored or default); an item that is off (enabledBy) is not written and is not checked. */
export function firstProblem(groups, valueOf) {
  for (const g of groups) {
    for (const { field, control } of g.cells) {
      if (field.enabledBy && !valueOf(field.enabledBy)) continue
      const v = valueOf(field.key, field)
      if (control === 'number' && outOfRange(field, v)) return { key: field.key, kind: 'range' }
      if (control === 'args' && customArgsProblem(v)) return { key: field.key, kind: 'args' }
    }
  }
  return null
}
