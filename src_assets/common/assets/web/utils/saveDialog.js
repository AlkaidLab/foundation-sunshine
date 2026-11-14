// A save dialog answers with no path when the person closes it. That is a quiet
// no-op, not an error: the caller gets null and writes nothing. A real failure
// (the dialog channel, the write) still rejects with the reason the caller shows.
export const SAVE_CANCELLED = 'cancelled'

/** The control panel's cancel sentinel, or the DOM's AbortError from a file picker. */
export function isSaveCancelled(error) {
  if (error === null || error === undefined) return false
  if (typeof error === 'string') return error === SAVE_CANCELLED
  return error.name === 'AbortError' || error.message === SAVE_CANCELLED
}

/** A control panel older than the command: the caller may use the browser download instead. */
export function isSaveCommandUnavailable(error) {
  const text = String(error?.message || error || '')
  return /(?:command\b.*\b(?:not found|not allowed)|unknown command|not implemented|not allowed by acl|not allowed on (?:window|origin))/i.test(text)
}

/** Runs a save command; resolves its answer, or null when the person cancelled. */
export async function saveThroughDialog(invoke, command, args) {
  try {
    return await invoke(command, args)
  } catch (error) {
    if (isSaveCancelled(error)) return null
    throw error
  }
}
