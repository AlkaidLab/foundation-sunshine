import { endBoot, registerModeHandler, useAppearance } from 'foundation-ui/vue'
import { setStoredTheme, setTheme } from './theme.js'
import { startEmbeddedRoot } from './embeddedRoot.js'

// The window's appearance (solid colour or wallpaper, the colours, light or dark) is applied once at start on every page of the Web UI,
// the way the control panel does it, so a wallpaper that was chosen stays after a reload and is on every page. The Web UI keeps its own
// light/dark switch: the appearance's mode drives it, and the first start takes over what the switch stored before.
// Inside the control panel's frame no picture is applied: the panel paints the wallpaper and the frame stays transparent; the frame wears the panel's
// colours, its light or dark choice and its "a picture is behind me" state (utils/embeddedRoot.js).
const embedded = () => window.isTauri === true && window.parent !== window

export async function startAppearance() {
  if (embedded()) { startEmbeddedRoot(); return }
  const key = 'foundation-ui.appearance'
  try {
    const stored = localStorage.getItem('theme')
    if (localStorage.getItem(key) === null && (stored === 'light' || stored === 'dark')) localStorage.setItem(key, JSON.stringify({ mode: stored }))
    // a picture chosen with the earlier background feature becomes the wallpaper of the shared appearance
    const legacy = localStorage.getItem('customBackground')
    if (localStorage.getItem(key) === null && legacy && /^(data:image|https?:)/.test(legacy) && !localStorage.getItem('foundation-ui.appearance.image')) {
      localStorage.setItem('foundation-ui.appearance.image', legacy)
      localStorage.setItem(key, JSON.stringify({ background: 'wallpaper', source: 'file', mode: stored === 'light' || stored === 'dark' ? stored : 'system' }))
    }
  } catch { /* storage unavailable: the defaults apply */ }
  registerModeHandler((mode) => {
    const theme = mode === 'system' ? 'auto' : mode
    setStoredTheme(theme)
    setTheme(theme)
  })
  await useAppearance().init()
  await new Promise((resolve) => setTimeout(resolve, 900)) // the wallpaper layer of the app fades in under the boot layers (700 ms)
  endBoot() // the sign-in page painted its first frame from the last record of this browser: the real appearance has arrived, the copy fades out
}
