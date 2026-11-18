// The display of a game app: 跟随串流 (the default) or 固定. A pure description (no Vue, no network) shared by the editor page and its tests.
// The host owns the meaning (src/sparxie/stream_display.h): follow = the game runs fullscreen at the size of the display that is streamed, resolved at every launch;
// fixed = the person's own size and window mode. Stored in the app's sparxie block as { mode, resolution, window, resolutionIndex } with every value a string.

import { RESOLUTIONS } from './model.js'
import { DISPLAY_GAMES, zzzResolutions } from './gameFields.js'

export const hasDisplay = (game) => DISPLAY_GAMES.includes(game)

const WINDOWS = ['fullscreen', 'borderless', 'windowed']
const SIZE = /^\s*(\d{3,5})\s*[x×X]\s*(\d{3,5})\s*$/
export const sizeText = (w, h) => `${w}×${h}`
export function parseSize(text) {
  const m = SIZE.exec(String(text ?? ''))
  return m ? { width: Number(m[1]), height: Number(m[2]) } : null
}

/** What is stored (or nothing) as the one shape the page works on: follow without any size, or fixed with its values */
export function normalizeDisplay(value) {
  if (!value || typeof value !== 'object' || value.mode !== 'fixed') return { mode: 'follow' }
  const out = { mode: 'fixed' }
  const size = parseSize(value.resolution)
  if (size) out.resolution = sizeText(size.width, size.height)
  out.window = WINDOWS.includes(value.window) ? value.window : 'fullscreen'
  const index = String(value.resolutionIndex ?? '')
  if (/^\d{1,3}$/.test(index)) out.resolutionIndex = index
  return out
}

/** The window modes a title offers: every title with a 显示 section has the three. Unity titles (and Endfield, a Unity game) get the borderless one as -popupwindow on the command line with the window in the
 *  registry (src/sparxie/stream_display.cpp); the Unreal titles have a borderless mode of their own. */
export function windowChoices(game) {
  return hasDisplay(game) ? ['fullscreen', 'borderless', 'windowed'] : []
}

/** The sizes of the list for a fixed display: the usual ones, and the size of the display the host sees (it is the one a stream covers) */
export function resolutionChoices(game, screen, stored) {
  if (game === 'zzz') return zzzResolutions(screen).map((r) => ({ value: String(r.index), mode: r.mode, width: r.width, height: r.height, native: !!r.native }))
  const list = [...RESOLUTIONS]
  const w = Number(screen?.width)
  const h = Number(screen?.height)
  if (w > 0 && h > 0 && !list.includes(sizeText(w, h))) list.push(sizeText(w, h))
  const keep = parseSize(stored)
  if (keep && !list.includes(sizeText(keep.width, keep.height))) list.push(sizeText(keep.width, keep.height))
  return list.sort((a, b) => parseSize(a).width * parseSize(a).height - parseSize(b).width * parseSize(b).height).map((value) => ({ value }))
}

/** The fixed values a switch to 固定 starts from: the size the host sees, else 1920×1080; the full-screen window */
export function startFixed(game, screen) {
  if (game === 'zzz') return { mode: 'fixed', window: 'fullscreen', resolutionIndex: '0' }
  const w = Number(screen?.width)
  const h = Number(screen?.height)
  return { mode: 'fixed', window: 'fullscreen', resolution: w > 0 && h > 0 ? sizeText(w, h) : '1920×1080' }
}

/** What goes to the host: the same shape, every value a string; follow carries nothing else */
export const displayBody = (display) => normalizeDisplay(display)

/** The state a page compares for 「changed」: a follow display has no leftovers of a fixed one */
export const displayState = (display) => normalizeDisplay(display)

/** The one line that says what the display will be. `stream` is the host's answer while a stream runs ({ width, height }), else null; `screen` the display the host sees. */
export function effectiveLine(display, game, { stream = null, screen = null } = {}, C) {
  const d = normalizeDisplay(display)
  if (d.mode === 'follow') return stream?.width > 0 && stream?.height > 0 ? C.displayFollowNow(stream.width, stream.height) : C.displayFollowLater
  if (game === 'zzz') {
    const entry = resolutionChoices('zzz', screen).find((r) => r.value === (d.resolutionIndex ?? '0'))
    return entry ? C.displayFixedLine(sizeText(entry.width, entry.height), C.display[entry.mode]) : C.displayFixedLine('', C.display[d.window])
  }
  return C.displayFixedLine(d.resolution || '', C.display[d.window])
}
