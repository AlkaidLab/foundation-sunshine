import { computed, reactive, ref } from 'vue'
import { DS5_CARDS, isClientKey } from 'foundation-ui/settings/index.js'
import { isAdvancedWarningHidden, setAdvancedWarningHidden } from '../utils/advancedWarning.js'
import { localizedKeyLabel } from '../config/dualsenseI18n.js'
import { askConfirm } from './askDialog.js'

// The DualSense tuning is one store for every place that shows it: the gamepad page (status, presets, cards, previews),
// its sub-pages and the shell's save bar. The host snapshot and the values being edited live here, and load / save go
// through `/api/dualsense/tuning`; the pages only read and change values.
const snapshot = ref(null)
const edited = reactive({})
const dirty = reactive({})
const loading = ref(false)
const loaded = ref(false)
const loadError = ref('')
const loadWarning = ref('')
const saving = ref(false)
const saveMessage = ref('')
const selectedPreset = ref('custom')
// the advanced groups are only open once the warning was confirmed (or switched off); a reload never skips it
const advancedOpen = ref(false)

export const PRESETS = Object.freeze(['default', 'competitive', 'immersive', 'legacy'])
// the values of a preset by key name (the same four as before; unwired keys are skipped when it is applied)
const presetValues = Object.freeze({
  default: { 'ds5.haptics.hd.gain': 1, 'ds5.rumble.enabled': true, 'ds5.rumble.strength': 1, 'ds5.rumble.curve': 1, 'ds5.rumble.lowGain': 1, 'ds5.rumble.highGain': 1, 'ds5.rumble.noiseGate': 0.02, 'ds5.led.lightbar.mode': 'followGame', 'ds5.led.lightbar.colorR': 0, 'ds5.led.lightbar.colorG': 0, 'ds5.led.lightbar.colorB': 0, 'ds5.led.lightbar.brightness': 1 },
  competitive: { 'ds5.haptics.hd.gain': 1.1, 'ds5.rumble.enabled': true, 'ds5.rumble.strength': 1.2, 'ds5.rumble.curve': 1.2, 'ds5.rumble.lowGain': 0.95, 'ds5.rumble.highGain': 1, 'ds5.rumble.noiseGate': 0.015, 'ds5.led.lightbar.mode': 'fixed', 'ds5.led.lightbar.colorR': 0, 'ds5.led.lightbar.colorG': 92, 'ds5.led.lightbar.colorB': 255, 'ds5.led.lightbar.brightness': 0.8 },
  immersive: { 'ds5.haptics.hd.gain': 1.4, 'ds5.rumble.enabled': true, 'ds5.rumble.strength': 1.3, 'ds5.rumble.curve': 1.1, 'ds5.rumble.lowGain': 1, 'ds5.rumble.highGain': 1, 'ds5.rumble.noiseGate': 0.02, 'ds5.led.lightbar.mode': 'fixed', 'ds5.led.lightbar.colorR': 255, 'ds5.led.lightbar.colorG': 126, 'ds5.led.lightbar.colorB': 24, 'ds5.led.lightbar.brightness': 1 },
  legacy: { 'ds5.haptics.hd.gain': 0, 'ds5.rumble.enabled': true, 'ds5.rumble.strength': 1, 'ds5.rumble.curve': 0.5, 'ds5.rumble.lowGain': 1, 'ds5.rumble.highGain': 1, 'ds5.rumble.noiseGate': 0.006, 'ds5.led.lightbar.mode': 'followGame', 'ds5.led.lightbar.colorR': 0, 'ds5.led.lightbar.colorG': 0, 'ds5.led.lightbar.colorB': 0, 'ds5.led.lightbar.brightness': 1 },
})
// A fixed colour that is still black would switch the lightbar off, so the first time a person picks "fixed" they start from a visible blue instead.
const FIRST_FIXED_COLOR = Object.freeze({ r: 74, g: 158, b: 255 })

const keysById = computed(() => new Map((snapshot.value?.keys || []).map((key) => [key.id, key])))
const keysByName = computed(() => new Map((snapshot.value?.keys || []).map((key) => [key.key, key])))
const descriptor = (id) => keysById.value.get(id) || null
const isWired = (id) => descriptor(id)?.implemented === true
/** Value being edited by key name, or `fallback` while the snapshot does not hold the key. */
const valueOf = (name, fallback = 0) => {
  const key = keysByName.value.get(name)
  return key ? edited[key.id] : fallback
}
const dirtyCount = computed(() => Object.keys(dirty).filter((id) => dirty[id]).length)

const present = (id) => descriptor(id) !== null
// the everyday cards: wired rows first, then the ones this build does not apply yet (shown disabled, with the reason)
const cards = computed(() => DS5_CARDS.map((card) => {
  const keys = card.keys.filter(present).map(descriptor)
  return { ...card, rows: keys.filter((key) => key.implemented), pending: keys.filter((key) => !key.implemented) }
}).filter((card) => card.rows.length || card.pending.length))
const advancedGroups = computed(() => {
  const groups = []
  for (let group = 1; group <= 12; ++group) {
    const keys = (snapshot.value?.keys || []).filter((key) => key.tier === 'advanced' && key.group === group && key.implemented)
    if (keys.length) groups.push({ id: group, titleKey: `ds5.groups.${group}`, wired: keys, pending: [] })
  }
  return groups
})
// the keys this host build does not apply, kept apart from the groups: the client's own (shown as such) and the not yet available
const unavailable = computed(() => {
  const keys = (snapshot.value?.keys || []).filter((key) => key.implemented === false)
  return { client: keys.filter(isClientKey), pending: keys.filter((key) => !isClientKey(key)), total: keys.length }
})

const setValue = (id, value) => {
  edited[id] = value
  dirty[id] = true
  if (selectedPreset.value !== 'custom') selectedPreset.value = 'custom'
  saveMessage.value = ''
}
const setColor = ({ r, g, b }) => { setValue(82, r); setValue(83, g); setValue(84, b) }
const onValue = (id, value) => {
  if (id === 81 && value === 'fixed' && !Number(edited[82]) && !Number(edited[83]) && !Number(edited[84])) setColor(FIRST_FIXED_COLOR)
  setValue(id, value)
}
const resetKey = (id) => { const key = descriptor(id); if (key?.implemented) setValue(id, key.default) }

const detectPreset = () => {
  for (const [name, values] of Object.entries(presetValues)) {
    const followsGame = values['ds5.led.lightbar.mode'] === 'followGame'
    const matches = Object.entries(values).every(([keyName, value]) => {
      const key = keysByName.value.get(keyName)
      if (!key) return true
      if (followsGame && /lightbar\.color[RGB]$/.test(keyName)) return true
      return String(edited[key.id]) === String(value)
    })
    if (matches) { selectedPreset.value = name; return }
  }
  selectedPreset.value = 'custom'
}
const hydrate = (body) => {
  snapshot.value = body
  for (const key of body.keys || []) { edited[key.id] = key.value; delete dirty[key.id] }
  detectPreset()
  loadWarning.value = body.load_error || (body.load_warnings?.length ? body.load_warnings.join('; ') : '')
}

const browserStorage = () => { try { return window.localStorage } catch { return null } }
const warningHidden = () => isAdvancedWarningHidden(browserStorage())
const rememberWarning = (dontShowAgain) => { if (dontShowAgain) setAdvancedWarningHidden(browserStorage(), true) }

export function useDualsenseTuning(t = (key) => key, locale = 'en') {
  const load = async (force = false) => {
    if (loading.value || (loaded.value && !force)) return
    loading.value = true
    loadError.value = ''
    try {
      const response = await fetch('/api/dualsense/tuning', { headers: { Accept: 'application/json' } })
      const body = await response.json()
      if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`)
      hydrate(body)
      loaded.value = true
    } catch (error) {
      loadError.value = error instanceof Error ? error.message : String(error)
    } finally {
      loading.value = false
    }
  }
  const applyPreset = (name) => {
    const values = presetValues[name]
    if (!values) return
    for (const [keyName, value] of Object.entries(values)) {
      const key = keysByName.value.get(keyName)
      if (!key || !key.implemented) continue
      edited[key.id] = value
      dirty[key.id] = true
    }
    selectedPreset.value = name
    saveMessage.value = t('ds5.presets.applied', { name: t(`ds5.presets.${name}`) })
  }
  const restoreDefaults = () => {
    for (const key of snapshot.value?.keys || []) {
      if (!key.implemented || String(edited[key.id]) === String(key.default)) continue
      edited[key.id] = key.default
      dirty[key.id] = true
    }
    detectPreset()
    saveMessage.value = t('ds5.defaults_restored')
  }
  const discard = () => {
    for (const key of snapshot.value?.keys || []) { edited[key.id] = key.value; delete dirty[key.id] }
    detectPreset()
    saveMessage.value = ''
  }
  const save = async () => {
    const values = {}
    const expert = []
    for (const key of snapshot.value?.keys || []) {
      if (!dirty[key.id]) continue
      values[key.key] = edited[key.id]
      if (key.expert) expert.push(localizedKeyLabel(key, locale))
    }
    if (!Object.keys(values).length) { saveMessage.value = t('ds5.nothing_dirty'); return true }
    if (expert.length && !(await askConfirm(t('ds5.expert_confirm', { keys: expert.join(', ') }), { title: t('ds5.expert'), confirmText: t('ds5.save') }))) return false
    saving.value = true
    saveMessage.value = ''
    try {
      const response = await fetch('/api/dualsense/tuning', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
        body: JSON.stringify({ schema: 'ALK-DS5T/1', values }),
      })
      const body = await response.json()
      if ((response.ok || response.status === 409) && body.keys) hydrate(body)
      if (!response.ok) { saveMessage.value = body.error || t('ds5.save_failed'); return false }
      const applied = body.applied?.length || 0
      const unavailable = body.unavailable?.length || 0
      saveMessage.value = unavailable ? t('ds5.save_partial', { applied, unavailable }) : t('ds5.save_ok', { count: applied })
      return true
    } catch (error) {
      saveMessage.value = error instanceof Error ? error.message : String(error)
      return false
    } finally {
      saving.value = false
    }
  }
  return { load, applyPreset, restoreDefaults, discard, save }
}

export const useDualsenseShared = () => ({
  snapshot, edited, dirty, loading, loaded, loadError, loadWarning, saving, saveMessage, selectedPreset, advancedOpen,
  dirtyCount, cards, advancedGroups, unavailable, descriptor, isWired, valueOf, keysByName, setValue, setColor, onValue, resetKey,
  warningHidden, rememberWarning,
})
