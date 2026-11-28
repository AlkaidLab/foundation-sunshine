// Pure helpers of the home tiles: nothing here reads the network or the DOM.

// The kind of a paired client and its icon: ONE copy, in foundation-ui, so the control panel draws the same icons.
export { DEVICE_KINDS, DEVICE_RULES, NEUTRAL_DEVICE_ICON, deviceKindFor, deviceIconFor } from 'foundation-ui/device-icons'

/** Bitrate from kilobits per second as { value, unit }, or null. */
export function formatBitrate(kbps) {
  const n = Number(kbps)
  if (!Number.isFinite(n) || n <= 0) return null
  if (n < 1000) return { value: String(Math.round(n)), unit: 'kbps' }
  const mbps = n / 1000
  return { value: Number.isInteger(mbps) ? String(mbps) : mbps.toFixed(1), unit: 'Mbps' }
}

/** "12:14" or "1:02:14" from milliseconds, or ''. */
export function formatUptime(ms) {
  const n = Number(ms)
  if (!Number.isFinite(n) || n < 0) return ''
  const s = Math.floor(n / 1000)
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const pad = (x) => String(x).padStart(2, '0')
  return h ? `${h}:${pad(m)}:${pad(s % 60)}` : `${m}:${pad(s % 60)}`
}

const num = (v) => (typeof v === 'number' && Number.isFinite(v) ? v : null)

/**
 * The numbers of the active session out of /api/perf/current. The measured frame rate comes from the host's frame-rate
 * collection when it is ready, else from the latency window; every field is null when the host did not send it.
 */
export function pickPerf(snapshot, sessionId = null) {
  const list = Array.isArray(snapshot?.sessions) ? snapshot.sessions : []
  const active = list.filter((s) => s?.active)
  const item = active.find((s) => sessionId != null && s.session_id === sessionId) || active[0]
  if (!item) return null
  const latency = item.host_latency || {}
  return {
    sessionId: item.session_id,
    bitrateKbps: num(item.bitrate_kbps),
    measuredKbps: num(item.measured_bitrate_kbps), // the bytes the stream really sent (a newer host); bitrateKbps is the session's budget
    fps: num(item.frame_rates?.encoded?.fps) ?? num(latency.recent_fps),
    targetFps: num(item.fps),
    avgMs: num(latency.avg_ms),
    p95Ms: num(latency.p95_ms),
    lastMs: num(latency.last_ms),
    series: Array.isArray(latency.series_ms) ? latency.series_ms.filter((v) => Number.isFinite(v)) : [],
    encoder: item.encoder || '',
    capture: item.capture || '',
    frameRates: item.frame_rates && typeof item.frame_rates === 'object' ? item.frame_rates : null, // the stage rates as the host reports them
    pipeline: item.pipeline && typeof item.pipeline === 'object' ? item.pipeline : null, // the stages of the host pipeline (the 帧时间占用 tile), as the host reports them
    uptimeMs: num(item.uptime_ms),
  }
}

/** Counts of log levels and the text of the latest error out of useLogs().fancyLogs. */
export function countLogLevels(entries) {
  const out = { fatal: 0, error: 0, warning: 0, lastError: '' }
  for (const entry of entries || []) {
    const level = entry?.level
    if (level === 'Fatal') out.fatal += 1
    else if (level === 'Error') out.error += 1
    else if (level === 'Warning') out.warning += 1
    else continue
    if (level === 'Fatal' || level === 'Error') out.lastError = String(entry.value || '').replace(/^\w+:\s*/, '').trim()
  }
  return out
}

/** Whether a tile can be shown: the host answered for the source it needs. */
export function tileAvailable(def, host) {
  if (def.needs === 'local') return host.local !== false
  if (def.needs === 'windows') return host.platform === 'windows'
  return true
}
