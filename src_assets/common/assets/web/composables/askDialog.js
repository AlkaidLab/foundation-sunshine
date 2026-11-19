import { useDialogs } from 'foundation-ui/dialog'

/** A question as the Material 3 dialog of the window (never the browser's confirm): resolves true on the confirming button, false on cancel or Escape. */
export async function askConfirm(message, { title = '', confirmText = '', cancelText = '', danger = false, type = 'warning' } = {}) {
  try {
    await useDialogs().confirm(message, title, { confirmText, cancelText, danger, type })
    return true
  } catch {
    return false
  }
}
