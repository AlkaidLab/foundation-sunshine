import { useConfig } from './useConfig.js'

// The configuration page state outlives the page: moving to another tab or page and back keeps unsaved edits.
// `onLoaded` runs after every load of the model from the host: the unsaved state applies the edits it kept (see pendingSources.js).
let shared = null
let onLoaded = null
export const setConfigLoadedHook = (fn) => { onLoaded = fn }
export const useSharedConfig = () => {
  if (!shared) {
    const cfg = useConfig()
    const load = cfg.loadConfig
    shared = { ...cfg, loadConfig: async (...args) => { await load(...args); onLoaded?.() } }
  }
  return shared
}
