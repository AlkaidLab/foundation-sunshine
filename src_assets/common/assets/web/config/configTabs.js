// The encoder pages that exist on the platform: the children of the encoders group that `useConfig` kept.
// (The categories of the configuration and their order are the shared settings model of the foundation-ui package.)
export function encoderTabsFor(tabs = []) {
  const group = tabs.find((tab) => tab.type === 'group')
  return group?.children || []
}
