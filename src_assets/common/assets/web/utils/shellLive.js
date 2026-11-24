// Pieces of data from the host that the list pane's second lines (foundation-ui rowLines) and the Home tiles share. Only real host data goes in:
// a value that the host did not send stays null.

const toArray = (payload, ...keys) => {
  if (Array.isArray(payload)) return payload
  for (const key of keys) if (Array.isArray(payload?.[key])) return payload[key]
  return null
}

/** Paired clients from /api/clients/list: { total, streaming } or null when the answer has no list. */
export function summariseClients(payload) {
  const list = toArray(payload, 'named_certs', 'clients')
  if (!list) return null
  return { total: list.length, streaming: list.filter((c) => c?.streaming && c?.enabled !== false).length, list }
}

/** Number of applications from /api/apps, or null. */
export function countApps(payload) {
  const list = toArray(payload, 'apps')
  return list ? list.length : null
}

// The host reports its active encoder with the internal name; this table only fixes the spelling.
const ENCODER_NAMES = { nvenc: 'NVENC', amd: 'AMF', amf: 'AMF', quicksync: 'Quick Sync', qsv: 'Quick Sync', vaapi: 'VA-API', videotoolbox: 'VideoToolbox', software: 'Software', vulkan: 'Vulkan' }

/** Display name of config.active_encoder, or '' when the host reports none. */
export function encoderLabel(value) {
  const raw = String(value ?? '').trim()
  if (!raw) return ''
  return ENCODER_NAMES[raw.toLowerCase()] || raw
}
