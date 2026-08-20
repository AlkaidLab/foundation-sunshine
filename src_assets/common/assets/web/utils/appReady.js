export const WEBUI_READY_MESSAGE = 'webui-ready'

export const markWebUiReady = (windowObject = window, documentObject = document) => {
  if (documentObject.documentElement) {
    documentObject.documentElement.dataset.sunshineReady = 'true'
  }

  const isEmbeddedTauri = Boolean(windowObject.isTauri || windowObject.__TAURI__)
    && windowObject.parent
    && windowObject.parent !== windowObject

  if (isEmbeddedTauri) {
    windowObject.parent.postMessage({ type: WEBUI_READY_MESSAGE }, '*')
  }
}
