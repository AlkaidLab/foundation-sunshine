import { getBootstrapConfig } from '../config/bootstrapData.js'
import { apiJson } from '../utils/apiFetch.js'

// The ONE place the Web UI reads the host's data from. Today it is the host's HTTP API on this computer, with the last answer of each topic kept so a page draws from it at once
// (stale-while-revalidate: `peek` is the kept answer, `refresh` the fresh one). The dedicated channel between the host and the panel (card panel-host-channel: `hostChannel.subscribe(topic)` and
// `hostChannel.call(command, args)`) replaces the inside of this file when it lands; no page imports anything else for host data.
const KEEP = Object.freeze({ config: 'foundation.config.last', apps: 'foundation.apps.last' })
const FETCH = Object.freeze({ config: () => getBootstrapConfig(), apps: () => apiJson('/api/apps') })

export const HOST_TOPICS = Object.freeze(Object.keys(KEEP))
export const keepKey = (topic) => KEEP[topic]

const storage = () => { try { return globalThis.localStorage ?? null } catch { return null } }

export const hostData = {
  /** The kept answer of a topic (what the host said last), or null. Synchronous: a page's first render never waits for the host. */
  peek(topic) {
    try { const value = JSON.parse(storage()?.getItem(KEEP[topic]) || 'null'); return value && typeof value === 'object' ? value : null } catch { return null }
  },
  /** The fresh answer of a topic; it is kept for the next start. Rejects when the host does not answer. */
  async refresh(topic) {
    const value = await FETCH[topic]()
    try { if (value && typeof value === 'object') storage()?.setItem(KEEP[topic], JSON.stringify(value)) } catch { /* storage unavailable or full: the next read is the fresh one */ }
    return value
  },
  /** `cb(value, { stale })` with the kept answer at once (stale), then with the fresh one; returns the function that stops it. The shape of hostChannel.subscribe. */
  subscribe(topic, cb) {
    let live = true
    const kept = this.peek(topic)
    if (kept) cb(kept, { stale: true })
    this.refresh(topic).then((value) => { if (live) cb(value, { stale: false }) }).catch(() => {})
    return () => { live = false }
  },
}
