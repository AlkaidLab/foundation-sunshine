import { reactive, computed } from 'vue'
import { ABSENT, createClient } from '../services/sparxieClient.js'

// The state of the Sparxie component for the pages of the Web UI: present or not, whether Sparxie is open, the state of its storage and whether the statement about injection was shown.
// Absent means the component is not installed (or the host does not have it): the pages then show nothing of it, not even an entry in the add menu.
const state = reactive({ loaded: false, ...ABSENT, store: { ...ABSENT.store } })
let client = createClient()
let inflight = null

export function setSparxieClient(next) { client = next || createClient(); inflight = null }
export const sparxieClient = () => client

/** Asks the host again; concurrent callers share one request. Never throws: a failed read leaves the previous state. */
export function refreshSparxie() {
  if (!inflight) {
    inflight = client.status()
      .then((s) => { Object.assign(state, s, { loaded: true }) })
      .catch(() => { state.loaded = true })
      .finally(() => { inflight = null })
  }
  return inflight
}

export function useSparxie() {
  // present: the component is installed (touch launch); available: the host has the game routes at all (a game app can be created without Sparxie)
  return { state, client: () => client, refresh: refreshSparxie, present: computed(() => state.loaded && state.installed), available: computed(() => state.loaded && state.available) }
}
