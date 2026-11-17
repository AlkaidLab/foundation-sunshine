import { watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { saveSettingKey, useNotify } from 'foundation-ui/vue'
import { apiJson } from '../utils/apiFetch.js'
import { useSharedConfig } from './configStore.js'
import { filterValidFps, serializeFps, serializeResolutions } from './useConfig.js'

// The stream parameters people change most (the resolutions and frame rates the host offers, the bitrate ceiling) apply when they are changed: one decision, not a change and a save. The key is
// written to the host on its own (the same saving path as the pinned tiles of the overview: the host's configuration with that key changed), the page takes the value as saved, and the snackbar
// offers to undo it. A key that cannot be written stays an unsaved change, for the bar of unsaved changes.
const hostApi = { getJson: (path) => apiJson(path), postJson: (path, body) => apiJson(path, { method: 'POST', body }) }
const DELAY = 700

export function useApplyNow() {
  const { t, te } = useI18n()
  const { config, fps, resolutions, markKeySaved, setKey } = useSharedConfig()
  const notify = useNotify()
  const read = {
    fps: () => serializeFps(filterValidFps([...(fps.value || [])])),
    resolutions: () => serializeResolutions(resolutions.value || []),
    max_bitrate: () => String(Number(config.value?.max_bitrate ?? 0) || 0), // an unset ceiling is 0 (the client decides): the loaded model fills it in, which is not a change
  }
  const nameOf = (key) => (te(`config.${key}`) ? t(`config.${key}`) : t('shell.display_modes'))
  const applied = {} // the value the host has, by key
  const timers = {}

  async function write(key, value, undoTo) {
    const result = await saveSettingKey(hostApi, key, value)
    if (!result.ok) { notify.snack(t('shell.apply_failed', { name: nameOf(key) })); return false }
    markKeySaved(key, value)
    applied[key] = value
    notify.snack(t('shell.applied', { name: nameOf(key) }), {
      action: undoTo !== undefined ? t('shell.undo') : '',
      onAction: undoTo === undefined ? null : async () => { if (await write(key, undoTo)) setKey(key, undoTo) }, // the page's own model follows the undone value
      id: `apply-${key}`,
    })
    return true
  }

  let model = null
  watch(() => (config.value ? Object.keys(read).map((k) => read[k]()) : null), (now) => {
    if (!now) return
    // a model loaded again from the host: what it holds is what the host has, not a change of the person
    if (config.value !== model) { model = config.value; for (const k of Object.keys(applied)) delete applied[k]; for (const k of Object.keys(timers)) clearTimeout(timers[k]) }
    Object.keys(read).forEach((key, i) => {
      if (applied[key] === undefined) { applied[key] = now[i]; return } // the first look is what the host has
      if (now[i] === applied[key]) return
      clearTimeout(timers[key])
      timers[key] = setTimeout(() => { const before = applied[key]; if (read[key]() !== before) void write(key, read[key](), before) }, DELAY)
    })
  }, { immediate: true, deep: true })
}
