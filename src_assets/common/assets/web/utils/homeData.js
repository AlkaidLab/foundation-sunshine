// Pure helpers of the home tiles: nothing here reads the network or the DOM.

// The kind of a paired client and its icon. The host reports the system a client runs on (`os`: android, ios, tvos, macos, windows, linux, from the client's user agent);
// it does not report a device type. The system decides what the device can be, the name the person gave refines the shape within it: an Android device is a phone unless its name says
// tablet, TV or handheld; an iOS device a phone unless it says iPad; a Mac is a Mac; Windows and Linux are PCs unless the name says handheld. Without a reported system the name alone decides
// (a keyword table, checked in this order: a TV box named "Android TV" must not become a phone); no match gives the neutral device icon.
export const DEVICE_KINDS = { tv: 'tv', handheld: 'sports_esports', vr: 'view_in_ar', tablet: 'tablet', phone: 'mobile', mac: 'laptop_mac', computer: 'desktop_windows' }
export const DEVICE_RULES = [
  { kind: 'tv', pattern: /\b(tv|apple ?tv|android ?tv|fire ?tv|chromecast|shield)\b|电视|客厅/i },
  { kind: 'handheld', pattern: /steam ?deck|rog ?ally|legion ?go|switch|handheld|掌机/i },
  { kind: 'vr', pattern: /quest|vision ?pro|\bvr\b|headset|pico/i },
  { kind: 'tablet', pattern: /ipad|tablet|\btab\b|\btab ?[a-z]?\d|matepad|mi ?pad|\bpad\b|\bpad ?\d|平板/i },
  { kind: 'phone', pattern: /iphone|pixel|galaxy ?[sazmf]|android|phone|redmi|xiaomi|oneplus|huawei|honor|vivo|oppo|iqoo|realme|meizu|nubia|motorola|手机/i },
  { kind: 'mac', pattern: /macbook|imac|mac ?mini|mac ?studio|\bmac\b|macos/i },
  { kind: 'computer', pattern: /laptop|notebook|thinkpad|surface|zenbook|desktop|\bpc\b|windows|linux|ubuntu|workstation|台式|电脑|笔记本/i },
]
const OS_KINDS = { android: ['phone', 'tablet', 'tv', 'handheld', 'vr'], ios: ['phone', 'tablet'], tvos: ['tv'], harmony: ['phone', 'tablet', 'tv'], macos: ['mac'], windows: ['computer', 'handheld', 'tablet'], linux: ['computer', 'handheld'] }
const OS_DEFAULT = { android: 'phone', ios: 'phone', tvos: 'tv', harmony: 'phone', macos: 'mac', windows: 'computer', linux: 'computer' }
export const NEUTRAL_DEVICE_ICON = 'devices'

/** The kind of a device ('phone' | 'tablet' | 'computer' | 'mac' | 'handheld' | 'tv' | 'vr') from its reported system and its name; '' when neither says. */
export function deviceKindFor(name, os = '') {
  const text = String(name ?? '')
  const byName = DEVICE_RULES.find((rule) => rule.pattern.test(text))?.kind || ''
  const system = String(os ?? '').toLowerCase()
  if (!OS_DEFAULT[system]) return byName
  return OS_KINDS[system].includes(byName) ? byName : OS_DEFAULT[system]
}

export function deviceIconFor(name, os = '') {
  const kind = deviceKindFor(name, os)
  if (kind === 'mac' && /imac|mac ?mini|mac ?studio/i.test(String(name ?? ''))) return 'desktop_mac'
  return DEVICE_KINDS[kind] || NEUTRAL_DEVICE_ICON
}

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
