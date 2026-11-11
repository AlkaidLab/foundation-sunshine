import { resolveControlPanelOrigin } from './nativeRtxHdrBridge.js'

export function openNativeDisplaySettings(
  windowObject = globalThis.window,
  documentObject = globalThis.document,
) {
  if (!windowObject?.isTauri || windowObject.parent === windowObject) return false
  const origin = resolveControlPanelOrigin(
    documentObject?.referrer,
    windowObject.location?.ancestorOrigins?.[0],
  )
  if (!origin) return false
  windowObject.parent.postMessage(
    { type: 'native-vdd-open-request', source: 'sunshine-webui' },
    origin,
  )
  return true
}
