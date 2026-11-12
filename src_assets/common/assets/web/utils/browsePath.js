// The chooser for a path setting. Only where the product owns the file system the host reads: the control panel
// (Tauri) opens its native dialog. A browser cannot give the host a path, so there the function is null and the
// row is the field alone.
export function browsePath(kind = 'file', titleOf = () => '') {
  const dialog = typeof window !== 'undefined' ? window.__TAURI__?.dialog : null
  if (!dialog?.open) return null
  return async () => {
    const chosen = await dialog.open({ directory: kind === 'folder', multiple: false, title: titleOf() })
    return typeof chosen === 'string' ? chosen : ''
  }
}
