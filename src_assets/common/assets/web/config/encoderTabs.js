const ENCODER_TAB_MATCHERS = [
  { tabId: 'nv', pattern: /(?:nvenc|cuda)/i },
  { tabId: 'qsv', pattern: /(?:quicksync|qsv)/i },
  { tabId: 'amd', pattern: /(?:amdvce|amf)/i },
  { tabId: 'vt', pattern: /videotoolbox/i },
  { tabId: 'sw', pattern: /(?:software|libx26[45])/i },
]

const ADAPTER_TAB_MATCHERS = [
  { tabId: 'nv', pattern: /(?:nvidia|geforce|quadro|tesla)/i },
  { tabId: 'amd', pattern: /(?:amd|radeon)/i },
  { tabId: 'qsv', pattern: /(?:intel|\barc\b|iris|uhd)/i },
  { tabId: 'vt', pattern: /apple/i },
]

const PLATFORM_DEFAULT_TABS = {
  macos: 'vt',
}

const matchAvailableTab = (value, matchers, availableTabs) => {
  if (!value) return null
  const match = matchers.find(({ pattern }) => pattern.test(String(value)))
  return match && availableTabs.has(match.tabId) ? match.tabId : null
}

const adapterName = (adapter) => (typeof adapter === 'string' ? adapter : adapter?.name || '')

export function getPreferredEncoderTab({
  activeEncoder = '',
  configuredEncoder = '',
  selectedAdapter = '',
  adapters = [],
  platform = '',
  availableTabIds = [],
} = {}) {
  const availableTabs = new Set(availableTabIds)
  if (!availableTabs.size) return null

  const encoderTab =
    matchAvailableTab(activeEncoder, ENCODER_TAB_MATCHERS, availableTabs) ||
    matchAvailableTab(configuredEncoder, ENCODER_TAB_MATCHERS, availableTabs)
  if (encoderTab) return encoderTab

  const selectedAdapterTab = matchAvailableTab(selectedAdapter, ADAPTER_TAB_MATCHERS, availableTabs)
  if (selectedAdapterTab) return selectedAdapterTab

  for (const adapter of adapters) {
    const detectedTab = matchAvailableTab(adapterName(adapter), ADAPTER_TAB_MATCHERS, availableTabs)
    if (detectedTab) return detectedTab
  }

  const platformDefault = PLATFORM_DEFAULT_TABS[platform]
  return (availableTabs.has(platformDefault) && platformDefault) || availableTabIds[0]
}
