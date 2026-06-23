const CACHE_PREFIX = 'sunshine-ai-cache'
const DEFAULT_MAX_ENTRIES = 500
const DEFAULT_TTL_MS = 30 * 24 * 60 * 60 * 1000

const memoryStores = new Map()

function getStorage(namespace) {
  try {
    if (typeof localStorage !== 'undefined') {
      return {
        getItem: (key) => localStorage.getItem(key),
        setItem: (key, value) => localStorage.setItem(key, value),
      }
    }
  } catch {
    // Restricted browsing contexts can throw when localStorage is accessed.
  }

  if (!memoryStores.has(namespace)) {
    memoryStores.set(namespace, new Map())
  }
  const store = memoryStores.get(namespace)
  return {
    getItem: (key) => store.get(key) || null,
    setItem: (key, value) => store.set(key, value),
  }
}

function stableStringify(value) {
  if (Array.isArray(value)) {
    return `[${value.map(stableStringify).join(',')}]`
  }
  if (value && typeof value === 'object') {
    return `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${stableStringify(value[key])}`).join(',')}}`
  }
  return JSON.stringify(value)
}

function hashKey(value) {
  const input = stableStringify(value)
  let hash = 2166136261
  for (let i = 0; i < input.length; i += 1) {
    hash ^= input.charCodeAt(i)
    hash = Math.imul(hash, 16777619)
  }
  return (hash >>> 0).toString(36)
}

function loadStore(namespace, version) {
  const storage = getStorage(namespace)
  const storageKey = `${CACHE_PREFIX}:${namespace}:${version}`
  const empty = { entries: {} }

  try {
    const raw = storage.getItem(storageKey)
    if (!raw) {
      return { storage, storageKey, data: empty }
    }
    const parsed = JSON.parse(raw)
    if (parsed && typeof parsed === 'object' && parsed.entries && typeof parsed.entries === 'object') {
      return { storage, storageKey, data: parsed }
    }
  } catch {
    // Ignore malformed cache data and start fresh.
  }

  return { storage, storageKey, data: empty }
}

function saveStore(storage, storageKey, data) {
  try {
    storage.setItem(storageKey, JSON.stringify(data))
  } catch {
    // Cache writes are best-effort; scraping should continue when storage is full or unavailable.
  }
}

function pruneEntries(entries, now, ttlMs, maxEntries) {
  const fresh = Object.entries(entries).filter(([, entry]) => entry && now - Number(entry.updatedAt || 0) <= ttlMs)
  fresh.sort(([, a], [, b]) => Number(b.updatedAt || 0) - Number(a.updatedAt || 0))
  return Object.fromEntries(fresh.slice(0, maxEntries))
}

export function createAiCache(namespace, { version = 'v1', ttlMs = DEFAULT_TTL_MS, maxEntries = DEFAULT_MAX_ENTRIES } = {}) {
  return {
    makeKey(payload) {
      return hashKey(payload)
    },

    get(key) {
      const now = Date.now()
      const { storage, storageKey, data } = loadStore(namespace, version)
      const entry = data.entries[key]
      if (!entry) return undefined

      if (now - Number(entry.updatedAt || 0) > ttlMs) {
        delete data.entries[key]
        saveStore(storage, storageKey, data)
        return undefined
      }

      return entry.value
    },

    set(key, value) {
      const now = Date.now()
      const { storage, storageKey, data } = loadStore(namespace, version)
      data.entries[key] = { value, updatedAt: now }
      data.entries = pruneEntries(data.entries, now, ttlMs, maxEntries)
      saveStore(storage, storageKey, data)
    },
  }
}

export const __aiCacheTestUtils = {
  stableStringify,
  hashKey,
  clearMemoryStores() {
    memoryStores.clear()
  },
}
