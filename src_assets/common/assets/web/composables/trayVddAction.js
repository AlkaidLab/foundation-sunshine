import { apiJson } from '../utils/apiFetch.js'

// The write path of the two tray-owned virtual-display keys. The full-settings save protects them on the host, so the
// control writes them with the same tray action the tray's own toggle uses: POST /api/tray/action checks the driver
// prerequisite and updates the running flag and the file together. The route accepts the credentials the Web UI already
// uses (a request from this machine needs none while no credentials are configured), so a plain browser can call it
// without any widened access, and a refusal (for example the missing driver) comes back as the error string below.
const TRAY_ACTION_OF = Object.freeze({
  vdd_keep_enabled: 'vdd_toggle_keep_enabled',
  vdd_headless_create: 'vdd_toggle_headless_create',
})

/** Writes one tray-owned virtual-display key through the tray action; rejects with the error the host returned. */
export async function writeTrayVddKey(key, enabled) {
  const action = TRAY_ACTION_OF[key]
  if (!action) throw new Error(`not a tray-owned virtual-display key: ${key}`)
  const result = await apiJson('/api/tray/action', { method: 'POST', body: { action, enabled: !!enabled } })
  if (result?.status !== true) throw new Error(result?.error || 'the host refused the virtual-display change')
  return result
}

/** Runs one of the tray's other virtual-display actions (`vdd_create`, `vdd_destroy`) through the same route; rejects with the host's error. */
export async function writeTrayVddAction(action) {
  const result = await apiJson('/api/tray/action', { method: 'POST', body: { action } })
  if (result?.status !== true) throw new Error(result?.error || 'the host refused the virtual-display change')
  return result
}
