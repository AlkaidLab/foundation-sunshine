import { createReachability } from 'foundation-ui/vue/guide.js'
import { apiFetch } from '../utils/apiFetch.js'

// The reachability check of the host (POST /api/network/check, GET /api/network/reachability): one state for the card of 主机设置 › 网络, the first-run step and the home tile.
// The host does the check itself; a host without the API answers 404, or closes the connection with 444, and the screens say so. Every request carries the signal of its timeout.
let shared = null
export function useReach() {
  if (!shared) {
    shared = createReachability({
      request: async (path, init = {}) => {
        const response = await apiFetch(path, init)
        return { ok: response.ok, status: response.status, json: await response.json().catch(() => null) }
      },
    })
  }
  return shared
}
