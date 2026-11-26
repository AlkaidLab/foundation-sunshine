// These host defaults are shared by config loading and the configuration difference list.
export const NVENC_DEFAULTS = Object.freeze({
  nvenc_preset: 4,
  nvenc_spatial_aq: 'enabled',
})

// The encoder pages that exist on the platform: the children of the encoders group that `useConfig` kept.
// (The categories of the configuration and their order are the shared settings model of the foundation-ui package.)
export function encoderTabsFor(tabs = []) {
  const group = tabs.find((tab) => tab.type === 'group')
  return group?.children || []
}
