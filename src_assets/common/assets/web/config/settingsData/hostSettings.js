// Host-specific additions to the pinned shared settings model. Keep the shared
// definitions intact so adding a host option does not require a UI-library fork.
import { DISPLAY_TREE as sharedTree, SETTING_META as sharedMeta, homesOf as sharedHomesOf, isChanged as sharedIsChanged } from 'foundation-ui/settings/index.js'

const edidKey = 'vdd_edid_native_resolution'
export const SETTING_META = Object.freeze({
  ...sharedMeta,
  [edidKey]: Object.freeze({ tier: 3, control: 'sheet', def: 'default' }),
})
/** Extend only the virtual-display page; the shared frozen tree remains unchanged. */
export const DISPLAY_TREE = Object.freeze(sharedTree.map(place => place.place !== 'virtual' ? place : Object.freeze({
  ...place,
  sections: Object.freeze([...place.sections, Object.freeze({ id: 'advanced', keys: Object.freeze([edidKey]), expander: true })]),
})))

/** Return the host option's location, delegating all existing keys to the shared tree. */
export const homesOf = key => key === edidKey ? [{ place: 'virtual', section: 'advanced' }] : sharedHomesOf(key)
/** Locate a setting in the host's extended display tree, or return null. */
export const homeOf = key => homesOf(key)[0] || null

/** Include the host-only preference in configuration differences and default resets. */
export const isChanged = (key, stored, def) => key === edidKey
  ? stored !== undefined && stored !== null && stored !== '' && String(stored) !== String(def ?? 'default')
  : sharedIsChanged(key, stored, def)
