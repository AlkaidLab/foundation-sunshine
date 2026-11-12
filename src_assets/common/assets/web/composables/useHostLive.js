import { onBeforeUnmount, ref } from 'vue'
import { apiFetch, apiPostJson } from '../utils/apiFetch.js'
import { pickPerf } from '../utils/homeData.js'

const CONSUMER = 'web-home'
const LEASE_MS = 10000

/**
 * Live state of the host for the home tiles. Sessions and performance only answer on the host itself: a 403 turns
 * `local` to false and the polling for them stops (the tiles that need them are left out, nothing is faked).
 *  - sessions: /api/runtime/sessions every 2 s, every second while a session is running; the numbers of the session
 *    come from /api/perf/current, whose frame rates are only collected while a lease is held (renewed every 5 s,
 *    released when the session ends or the page closes).
 *  - hdr (hdr-calibration), mic (microphone/status), vdd (vdd/status): every 10 s, only for the tiles that are placed.
 * `want` returns the ids of the tiles that are on the page.
 */
export function useHostLive({ want, platform }) {
  const local = ref(null)
  const sessions = ref([])
  const perf = ref(null)
  const hdr = ref(null)
  const mic = ref(null)
  const vdd = ref(null)
  // CPU, GPU and memory of the host (/api/runtime/system); a host without the route (404) or a remote browser (403) leaves it null
  const system = ref(null)
  let sysTimer = 0
  let sysOff = false
  let timer = 0
  let auxTimer = 0
  let leaseAt = 0
  let leased = false
  let closed = false

  const get = async (path) => {
    const response = await apiFetch(path)
    if (response.status === 403 || response.status === 401) return { forbidden: true }
    if (!response.ok) return { error: true }
    return { data: await response.json().catch(() => null) }
  }

  async function release() {
    if (!leased) return
    leased = false
    try { await apiPostJson('/api/perf/frame-rates', { consumer_id: CONSUMER, enabled: false, lease_ms: 1000 }) } catch { /* lease expires by itself */ }
  }
  async function lease() {
    if (Date.now() - leaseAt < 5000) return
    leaseAt = Date.now()
    try { await apiPostJson('/api/perf/frame-rates', { consumer_id: CONSUMER, enabled: true, lease_ms: LEASE_MS }); leased = true } catch { leaseAt = Date.now() + 55000 /* fps falls back to the latency window; a refusal is not asked again every 5 s */ }
  }

  async function tick() {
    let next = 2000
    if (!document.hidden && local.value !== false) {
      try {
        const s = await get('/api/runtime/sessions')
        if (s.forbidden) { local.value = false; sessions.value = []; perf.value = null; return }
        if (s.data) {
          local.value = true
          sessions.value = Array.isArray(s.data.sessions) ? s.data.sessions : []
        }
        if (sessions.value.length) {
          next = 1000
          if (want().has('latency') || want().has('bitrate') || want().has('fps') || want().has('session')) {
            await lease()
            const p = await get('/api/perf/current')
            perf.value = p.data ? pickPerf(p.data, sessions.value[0]?.session_id) : perf.value
          }
        } else {
          perf.value = null
          await release()
        }
      } catch { /* host busy: try again next round */ }
    }
    if (!closed && local.value !== false) timer = setTimeout(tick, next)
  }

  async function aux() {
    if (!document.hidden) {
      const ids = want()
      const jobs = []
      if (ids.has('hdr') && local.value !== false) jobs.push(get('/api/runtime/hdr-calibration').then((r) => { hdr.value = r.data?.supported ? r.data : null }))
      if (ids.has('mic')) jobs.push(get('/api/microphone/status').then((r) => { mic.value = r.data && r.data.state !== 'unsupported' ? r.data : null }))
      if (ids.has('vdd') && platform() === 'windows') jobs.push(get('/api/vdd/status').then((r) => { vdd.value = r.data ? (r.data.data || r.data) : null }))
      await Promise.allSettled(jobs)
    }
  }

  async function sysTick() {
    if (!document.hidden && !sysOff) {
      const ids = want()
      if (ids.has('cpu') || ids.has('gpu') || ids.has('memory')) {
        try {
          const r = await get('/api/runtime/system')
          if (r.forbidden || (r.error && !r.data)) { sysOff = r.forbidden || sysOff; system.value = null }
          else if (r.data && r.data.supported) system.value = r.data
          else if (r.data) { system.value = null; sysOff = true }
        } catch { /* try again next round */ }
      }
    }
    if (!closed && !sysOff) sysTimer = setTimeout(sysTick, 3000)
  }

  const onHide = () => { if (document.hidden) void release() }
  timer = setTimeout(tick, 0)
  void aux()
  auxTimer = setInterval(aux, 10000)
  sysTimer = setTimeout(sysTick, 500)
  document.addEventListener('visibilitychange', onHide)
  window.addEventListener('pagehide', release)
  onBeforeUnmount(() => {
    closed = true
    clearTimeout(timer)
    clearInterval(auxTimer)
    clearTimeout(sysTimer)
    document.removeEventListener('visibilitychange', onHide)
    window.removeEventListener('pagehide', release)
    void release()
  })

  return { local, sessions, perf, hdr, mic, vdd, system, refreshAux: aux }
}
