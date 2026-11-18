import { createVddStore, vddStatusOf } from 'foundation-ui/vue'
import { apiJson } from '../utils/apiFetch.js'
import { useSharedConfig } from './configStore.js'
import { writeTrayVddKey } from './trayVddAction.js'

// The one store of the virtual display control for this product: the settings page and the home card use it, so what the
// person changes in one shows in the other. Normal keys live in the shared configuration (and are saved with it); the two
// tray-owned keys never go through the config save (the host protects them there), the store writes them through the tray
// action instead: the panel shell through its own invoke, a plain browser directly against the same authenticated route,
// and a refusal (for example the missing driver) becomes the store's error message.
let store = null
export function useVddSettings() {
  if (store) return store
  const shared = useSharedConfig()
  const invoke = typeof window !== 'undefined' ? window.__TAURI__?.core?.invoke : null
  const adapter = {
    read: () => shared.config.value || {},
    write: (patch) => { if (shared.config.value) Object.assign(shared.config.value, patch) },
    async commit() {
      if (!shared.config.value) await shared.loadConfig()
      if (!(await shared.save())) throw new Error(shared.saveError.value || shared.validationError.value || 'save failed')
    },
    async status() { return vddStatusOf(await apiJson('/api/vdd/status')) },
  }
  if (invoke) {
    adapter.setProtected = async (key, enabled) => {
      await invoke(key === 'vdd_keep_enabled' ? 'set_vdd_keep_enabled' : 'set_vdd_headless_create_enabled', { enabled })
    }
  }
  else {
    adapter.setProtected = (key, enabled) => writeTrayVddKey(key, enabled)
  }
  store = createVddStore(adapter)
  return store
}
