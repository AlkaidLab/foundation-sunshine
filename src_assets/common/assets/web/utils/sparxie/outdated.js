// An installed or just imported Sparxie older than the one the host is made for: a dialog says so and asks for a newer one. The host names the release
// (outdatedVersion); the pages that show Sparxie warn once for each, and an import of an old package warns every time.
const warned = new Set()

const dialog = async (message, title, options) => {
  const { useDialogs } = await import('foundation-ui/dialog')
  await useDialogs().alert(message, title, options)
}

/** `status` is { outdatedVersion, latestVersion }, `texts` the copy of the page. Resolves true when the dialog was asked for. */
export async function warnOutdated(status, texts, { always = false, alert = dialog } = {}) {
  const version = status?.outdatedVersion
  if (!version || (!always && warned.has(version))) return false
  warned.add(version)
  try {
    await alert(texts.outdatedText(version, status.latestVersion), texts.outdatedTitle, { type: 'warning', confirmText: texts.outdatedOk })
  } catch { /* closed with Escape */ }
  return true
}

export const resetOutdatedWarnings = () => warned.clear()
