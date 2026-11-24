export const CONTROL_PANEL_ORIGINS = new Set([
  'http://tauri.localhost',
  'https://tauri.localhost',
  'tauri://localhost',
  'http://localhost:8080',
  'https://localhost:8080',
])

export function normalizeControlPanelOrigin(url) {
  try {
    const parsed = new URL(url)
    return parsed.origin === 'null' ? `${parsed.protocol}//${parsed.host}` : parsed.origin
  } catch {
    return ''
  }
}

export function resolveControlPanelOrigin(referrer, ancestorOrigin) {
  return [referrer, ancestorOrigin]
    .map(normalizeControlPanelOrigin)
    .find((origin) => CONTROL_PANEL_ORIGINS.has(origin)) || ''
}

export function isTrustedRtxHdrBridgeMessage(event, parentWindow, controlPanelOrigin) {
  return Boolean(
    controlPanelOrigin
    && CONTROL_PANEL_ORIGINS.has(controlPanelOrigin)
    && event.source === parentWindow
    && event.origin === controlPanelOrigin
    && event.data?.source === 'sunshine-control-panel'
  )
}

export function createNativeRtxHdrBridge({ windowObject, documentObject, onAvailability }) {
  const controlPanelOrigin = resolveControlPanelOrigin(
    documentObject.referrer,
    windowObject.location.ancestorOrigins?.[0],
  )
  let available = false

  const handleMessage = (event) => {
    if (!isTrustedRtxHdrBridgeMessage(event, windowObject.parent, controlPanelOrigin)) return
    if (event.data.type === 'native-rtx-hdr-context') {
      available = event.data.available === true
      onAvailability(available)
    }
  }

  const requestContext = () => {
    if (windowObject.parent === windowObject || !controlPanelOrigin) return false
    windowObject.parent.postMessage(
      { type: 'native-rtx-hdr-context-request', source: 'sunshine-webui' },
      controlPanelOrigin,
    )
    return true
  }

  const openManager = () => {
    if (!available || !controlPanelOrigin) return false
    windowObject.parent.postMessage(
      {
        type: 'native-rtx-hdr-open-request',
        source: 'sunshine-webui',
        requestId: `${Date.now()}-${Math.random().toString(36).slice(2)}`,
      },
      controlPanelOrigin,
    )
    return true
  }

  return { controlPanelOrigin, handleMessage, requestContext, openManager }
}
