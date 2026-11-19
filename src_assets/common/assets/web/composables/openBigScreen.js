import { apiJson } from '../utils/apiFetch.js'

// The Open Big Screen tile and search action of the Web UI. The big screen (Foundation Desktop) opens on the machine the host runs on, so a browser on that
// machine can ask for it with the tray action route (POST /api/tray/action, the route the virtual-display switches use, open to a request from this machine);
// a browser anywhere else gets no tile and no action: absent, not disabled.
const LOOPBACK = /^(localhost|127\.|0\.0\.0\.0|::1|\[::1\])/i

/**
 * Whether this page can open the big screen.
 * @param {{ platform?: string, local?: boolean | null, hostname?: string }} where platform: the host's (the panel is a Windows component);
 *   local: the host's own answer that this request comes from its machine (true / false, null while unknown); hostname: the address of the page, used while local is unknown.
 */
export function canOpenBigScreen({ platform, local = null, hostname = '' } = {}) {
  if (platform !== 'windows') return false
  if (local === true) return true
  if (local === false) return false
  return LOOPBACK.test(String(hostname).trim())
}

/** Asks the host to open the big screen; rejects with the sentence the host gave when it cannot (the panel is not installed, nobody is signed in). */
export async function openBigScreenOnHost() {
  const result = await apiJson('/api/tray/action', { method: 'POST', body: { action: 'open_desktop' } })
  if (result?.status !== true) throw new Error(result?.error || 'the host refused to open the big screen')
  return result
}
