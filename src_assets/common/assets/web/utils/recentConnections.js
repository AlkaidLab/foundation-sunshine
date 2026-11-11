export const RECENT_CONNECTION_LIMIT = 8

export function getStreamingState(clients, failed = false) {
  if (failed) return 'unavailable'
  if (!Array.isArray(clients)) return 'loading'
  if (clients.some((client) => client?.streaming === true)) return 'streaming'
  if (clients.some((client) => client?.streaming !== false)) return 'unavailable'
  return 'idle'
}

export function getStreamingDestination(clients, copy) {
  const active = clients.filter((client) => client.streaming === true)
  if (!active.length) return null
  const name =
    active.length === 1 && typeof active[0].name === 'string' ? active[0].name.trim() : ''
  return {
    target:
      active.length > 1
        ? copy.arrivalMultiple.replace('{count}', String(active.length))
        : name || copy.arrivalUnnamed,
    key: JSON.stringify(active.map((client) => client.uuid).sort()),
  }
}

export function parseHomeClients(payload) {
  if (
    !['true', '1'].includes(String(payload?.status).toLowerCase()) ||
    !Array.isArray(payload?.named_certs)
  ) {
    throw new Error('Invalid client list response')
  }
  return payload.named_certs.filter(
    (client) => client && typeof client.uuid === 'string' && client.uuid,
  )
}

// Records are observations of real streaming sessions, never inferred from pairing.
// The host persists each session's end time (clients report `lastSessionEnded`),
// so history survives browsers and page closures; localStorage only adds what
// this WebUI happened to witness while open. Unpaired devices are removed on
// the next successful refresh.
export function updateRecentConnections(previous, clients, now = Date.now()) {
  const known = new Map(clients.map((client) => [client.uuid, client]))
  const records = new Map()
  for (const record of Array.isArray(previous) ? previous : []) {
    const client = known.get(record?.uuid)
    if (
      !client ||
      !Number.isFinite(record.lastSeen) ||
      record.lastSeen <= 0 ||
      record.lastSeen > now
    )
      continue
    records.set(record.uuid, {
      uuid: record.uuid,
      name: client.name || '',
      lastSeen: record.lastSeen,
    })
  }
  for (const client of clients) {
    if (client.streaming === true) {
      records.set(client.uuid, { uuid: client.uuid, name: client.name || '', lastSeen: now })
      continue
    }
    // The host's own session history outranks browser-local observation; a
    // paired client with neither is simply a device that never streamed here.
    const observed = records.get(client.uuid)?.lastSeen ?? 0
    const reported =
      Number.isFinite(client.lastSessionEnded) && client.lastSessionEnded > 0
        ? client.lastSessionEnded
        : 0
    const lastSeen = Math.min(now, Math.max(observed, reported))
    if (lastSeen > 0) {
      records.set(client.uuid, { uuid: client.uuid, name: client.name || '', lastSeen })
    }
  }
  return [...records.values()]
    .sort((a, b) => b.lastSeen - a.lastSeen)
    .slice(0, RECENT_CONNECTION_LIMIT)
}

export function readRecentConnections(storage, key) {
  try {
    const records = JSON.parse(storage.getItem(key) || '[]')
    return Array.isArray(records) ? records.slice(0, RECENT_CONNECTION_LIMIT) : []
  } catch {
    return []
  }
}

export function writeRecentConnections(storage, key, records) {
  try {
    storage.setItem(key, JSON.stringify(records))
  } catch {
    // Private browsing or a full quota must not prevent the home page loading.
  }
}

const RECENT_STORE_PREFIX = 'sunshine-recent-connections:'
const PRUNE_AFTER_DAYS = 90

// Stores orphaned by a host rename or a changed endpoint linger forever; drop
// siblings whose newest record is older than the retention window (or junk).
// Keys still in use — including other endpoints' live histories — survive.
export function pruneRecentConnectionStores(storage, activeKey, now = Date.now()) {
  let siblings
  try {
    siblings = []
    for (let index = 0; index < storage.length; index += 1) {
      const key = storage.key(index)
      if (key && key.startsWith(RECENT_STORE_PREFIX) && key !== activeKey) siblings.push(key)
    }
  } catch {
    return
  }
  const staleBefore = now - PRUNE_AFTER_DAYS * 86400000
  for (const key of siblings) {
    try {
      const records = JSON.parse(storage.getItem(key) || '[]')
      const newest = Array.isArray(records)
        ? records.reduce((acc, record) => Math.max(acc, Number.isFinite(record?.lastSeen) ? record.lastSeen : 0), 0)
        : 0
      if (newest < staleBefore) storage.removeItem(key)
    } catch {
      storage.removeItem(key)
    }
  }
}

// Recency tiers for the recent list. Returns '' when the record is too old
// (or invalid) for a relative label, so callers fall back to an absolute date.
export function formatRelativeSeen(timestamp, units, now = Date.now()) {
  if (!units || !Number.isFinite(timestamp) || timestamp <= 0) return ''
  const elapsed = now - timestamp
  if (elapsed < 0 || elapsed >= 7 * 86400000) return ''
  if (elapsed < 60000) return units.justNow
  if (elapsed < 3600000) return units.minutesAgo.replace('{n}', String(Math.floor(elapsed / 60000)))
  if (elapsed < 86400000) return units.hoursAgo.replace('{n}', String(Math.floor(elapsed / 3600000)))
  if (elapsed < 172800000) return units.yesterday
  return units.daysAgo.replace('{n}', String(Math.floor(elapsed / 86400000)))
}
