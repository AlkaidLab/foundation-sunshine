// The scan of the game libraries and of a directory through the host (POST /api/apps/scan, docs/api/apps-scan.md of the host), the same call for the Web UI in a browser and in the control panel.
// The host only reports candidates: the page adds the selected ones through the application save API. A host without the API answers 404 (501): the control panel then uses its native scan
// (Tauri) and a browser says the scan is unavailable.

/** The sources the host knows. */
export const SCAN_SOURCES = Object.freeze([
  'steam', 'epic', 'gog', 'xbox', 'ea', 'ubisoft', 'battlenet', 'riot', 'hoyoplay', 'wegame', 'sparxie',
])

export const SCAN_SOURCE_LABELS = Object.freeze({
  steam: 'Steam',
  epic: 'Epic Games',
  gog: 'GOG',
  xbox: 'Xbox',
  ea: 'EA app',
  ubisoft: 'Ubisoft Connect',
  battlenet: 'Battle.net',
  riot: 'Riot Client',
  hoyoplay: 'HoYoPlay',
  wegame: 'WeGame',
  sparxie: 'Sparxie',
})

/** The body of the request: the chosen library sources and/or a directory of the host. */
export function scanBody({ platforms = [], directory = '' } = {}) {
  const sources = [...new Set((platforms || []).filter((p) => SCAN_SOURCES.includes(p)))]
  const dir = String(directory || '').trim()
  const body = {}
  if (sources.length) body.sources = sources
  if (dir) body.directory = dir
  return body
}

/** One game of the answer as an application of the scan result sheet (the shape the native scan gives, so the sheet, the covers and the saving are the same). */
export function mapHostGame(game) {
  const launches = Array.isArray(game.launches) ? game.launches : []
  const mode = game.platform === 'sparxie' && launches.some((choice) => choice.id === 'touch')
    ? 'touch'
    : (launches.find((choice) => choice.launch)?.id || launches[0]?.id || '')
  const chosen = launches.find((choice) => choice.id === mode)
  return {
    name: game.name,
    cmd: (chosen && chosen.launch) || game.launch || game.exe || '',
    'working-dir': game.install_dir || '',
    'image-path': game.cover_hint || '',
    source_path: game.install_dir || game.id || '',
    'app-type': game.platform,
    'is-game': true,
    ...(game.exe ? { exe: game.exe } : {}),
    ...(game.region ? { region: game.region } : {}),
    ...(game.profile ? { profile: game.profile } : {}),
    ...(launches.length ? { launches, mode } : {}),
    sources: Array.isArray(game.sources) && game.sources.length ? game.sources : [game.platform].filter(Boolean),
  }
}

/** A Sparxie profile is saved by the same host routes as a new Sparxie app, not by the generic app list. */
export function sparxiePlan(game) {
  if (!game?.profile) return null
  if (game.mode === 'pc') return { kind: 'pc', path: game.exe || game.cmd || '', name: game.name || '', region: game.region || '' }
  if (game.mode === 'touch' || game['app-type'] === 'sparxie' || game.platform === 'sparxie') return { kind: 'touch', id: game.profile }
  return null
}

/** The result of the call: { ok, games (mapped), counts per platform, warnings } or { ok: false, unsupported, status } for a host without the API, a conflict or a refusal. */
export function readScan(status, data) {
  if (status === 404 || status === 501) return { ok: false, unsupported: true, status }
  if (status === 409) return { ok: false, busy: true, status }
  if (status < 200 || status >= 300 || !data || !Array.isArray(data.games)) return { ok: false, status, error: typeof data?.error === 'string' ? data.error : '' }
  const counts = {}
  for (const g of data.games) counts[g.platform] = (counts[g.platform] || 0) + 1
  return { ok: true, games: data.games.map(mapHostGame), counts, warnings: Array.isArray(data.warnings) ? data.warnings : [] }
}
