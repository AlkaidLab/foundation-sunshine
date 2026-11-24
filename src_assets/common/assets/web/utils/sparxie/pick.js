// The chooser of a path for the Sparxie pages.
//
// The host is a service: a dialog it opened would land in the service's profile, not in the person's session, so the host never opens one.
// Inside the control panel (a window of the person's session) the panel's own dialog is the chooser and the page sends the chosen path to the host.
// In a browser the page shows the host's read-only browse list instead (SparxieBrowse.vue); this returns null there.
export function nativePicker() {
  const dialog = typeof window !== 'undefined' ? window.__TAURI__?.dialog : null
  if (!dialog?.open) return null
  /** kind: 'file' (an .exe) | 'folder'. Resolves with the path, or '' when the person cancelled. `exes` are named in the file filter. */
  return async (kind, { title = '', exes = [] } = {}) => {
    const options = kind === 'folder'
      ? { directory: true, multiple: false, title }
      : { directory: false, multiple: false, title, filters: [{ name: exes.length ? exes.join(', ') : 'EXE', extensions: ['exe'] }] }
    const chosen = await dialog.open(options)
    return typeof chosen === 'string' ? chosen : ''
  }
}
