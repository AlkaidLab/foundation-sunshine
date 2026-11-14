// Small helpers of the diagnostics page that need nothing from the page: the system line of the host log and writing the bundle.

import { SAVE_CANCELLED, isSaveCancelled, isSaveCommandUnavailable, saveThroughDialog } from './saveDialog.js'

/** "Running with Windows 11 ..." is the host's own first lines of the log: the only place the host says which system it runs on. */
export const osFromLog = (text) => /^\[[^\]]+\]:\s*Info:\s*Running with (.+)$/m.exec(String(text || ''))?.[1]?.trim() || ''

const base64 = (bytes) => {
  let out = ''
  for (let i = 0; i < bytes.length; i += 0x8000) out += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
  return btoa(out)
}

/** Writes the bundle: the control panel's save dialog when this page runs inside it, otherwise a normal download. */
export async function saveBundle(name, bytes, win = globalThis.window) {
  const invoke = win?.__TAURI_INTERNALS__?.invoke
  if (typeof invoke === 'function') {
    try {
      const saved = await saveThroughDialog(invoke, 'save_binary_file', { contentBase64: base64(bytes), defaultName: name, filterName: 'Zip', extensions: ['zip'] })
      if (saved === null) throw new Error(SAVE_CANCELLED)
      return saved
    } catch (e) {
      if (isSaveCancelled(e)) throw new Error(SAVE_CANCELLED)
      // an older control panel has no such command: fall through to the download
      if (!isSaveCommandUnavailable(e)) throw e
    }
  }
  // The browser download has no cancel signal and nothing to show while it runs.
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/zip' }))
  const link = Object.assign(document.createElement('a'), { href: url, download: name })
  document.body.appendChild(link)
  link.click()
  document.body.removeChild(link)
  setTimeout(() => URL.revokeObjectURL(url), 10000)
  return ''
}
