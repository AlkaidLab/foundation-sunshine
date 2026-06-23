const OVERRIDE_STORAGE_KEY = 'sunshine-scan-overrides:v1'
const MAX_OVERRIDES = 1000

function getStorage() {
  if (typeof localStorage !== 'undefined') {
    return {
      getItem: (key) => localStorage.getItem(key),
      setItem: (key, value) => localStorage.setItem(key, value),
    }
  }

  if (!globalThis.__sunshineScanOverrideStore) {
    globalThis.__sunshineScanOverrideStore = new Map()
  }
  const store = globalThis.__sunshineScanOverrideStore
  return {
    getItem: (key) => store.get(key) || null,
    setItem: (key, value) => store.set(key, value),
  }
}

function normalizeKeyPart(value) {
  return String(value || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\s+/g, ' ')
    .toLowerCase()
}

function readOverrides() {
  try {
    const raw = getStorage().getItem(OVERRIDE_STORAGE_KEY)
    if (!raw) return { entries: {} }
    const parsed = JSON.parse(raw)
    if (parsed && typeof parsed === 'object' && parsed.entries && typeof parsed.entries === 'object') {
      return parsed
    }
  } catch {
    // Ignore malformed user override data and start clean.
  }

  return { entries: {} }
}

function writeOverrides(data) {
  const entries = Object.entries(data.entries || {})
  entries.sort(([, a], [, b]) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))
  data.entries = Object.fromEntries(entries.slice(0, MAX_OVERRIDES))

  try {
    getStorage().setItem(OVERRIDE_STORAGE_KEY, JSON.stringify(data))
  } catch {
    // Overrides are best-effort; adding apps should still succeed when storage is unavailable.
  }
}

export function getScanOverrideKey(app) {
  const sourcePath = normalizeKeyPart(app?.source_path || app?.sourcePath)
  if (sourcePath) return `source:${sourcePath}`

  const cmd = normalizeKeyPart(app?.cmd)
  if (cmd) return `cmd:${cmd}`

  const name = normalizeKeyPart(app?.name)
  const workingDir = normalizeKeyPart(app?.['working-dir'] || app?.working_dir)
  return name ? `name:${workingDir}:${name}` : ''
}

export function learnScanOverride(scannedApp, finalApp = scannedApp) {
  const key = getScanOverrideKey(scannedApp)
  if (!key) return false

  const data = readOverrides()
  data.entries[key] = {
    name: finalApp.name || scannedApp.name || '',
    cmd: finalApp.cmd || scannedApp.cmd || '',
    'working-dir': finalApp['working-dir'] || scannedApp['working-dir'] || scannedApp.working_dir || '',
    'image-path': finalApp['image-path'] || scannedApp['image-path'] || scannedApp.image_path || '',
    'canonical-name': finalApp['canonical-name'] || scannedApp['canonical-name'] || finalApp.name || scannedApp.name || '',
    'cover-search-terms': Array.isArray(scannedApp['cover-search-terms']) ? scannedApp['cover-search-terms'] : [],
    'is-game': scannedApp['is-game'] === true,
    updatedAt: Date.now(),
  }
  writeOverrides(data)
  return true
}

function applyOverride(app, override) {
  return {
    ...app,
    ...(override.name && { name: override.name }),
    ...(override.cmd && { cmd: override.cmd }),
    ...(override['working-dir'] && { 'working-dir': override['working-dir'] }),
    ...(override['image-path'] && { 'image-path': override['image-path'] }),
    ...(override['canonical-name'] && { 'canonical-name': override['canonical-name'] }),
    ...(override['cover-search-terms']?.length && { 'cover-search-terms': override['cover-search-terms'] }),
    'ai-confidence': 1,
    'ai-cover-confidence': override['image-path'] ? 1 : app['ai-cover-confidence'],
    'user-override': true,
    'ai-reason': 'User confirmed override',
  }
}

export function applyScanOverride(app) {
  const key = getScanOverrideKey(app)
  if (!key) return app

  const override = readOverrides().entries[key]
  if (!override) return app

  return applyOverride(app, override)
}

export function applyScanOverrides(apps) {
  if (!Array.isArray(apps)) return apps

  const overrides = readOverrides().entries || {}
  return apps.map((app) => {
    const key = getScanOverrideKey(app)
    return key && overrides[key] ? applyOverride(app, overrides[key]) : app
  })
}

export const __scanOverrideTestUtils = {
  clear() {
    if (globalThis.__sunshineScanOverrideStore) {
      globalThis.__sunshineScanOverrideStore.clear()
    }
  },
}
