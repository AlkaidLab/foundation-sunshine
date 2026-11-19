import i18n from './config/i18n.js'
import { markWebUiReady } from './utils/appReady.js'
import { installExternalLinkHandler } from './utils/helpers.js'
import { registerModeHandler } from 'foundation-ui/vue'
import { configureDevBackend } from 'foundation-ui/vue'
import { applyDevBackend } from './devBackendConfig.js'
import { setStoredTheme, setTheme } from './utils/theme.js'
import { installSessionGuard } from './utils/sessionGuard.js'
import { setPlatformI18n } from './platform-i18n.js'

// the GitHub flows of developer mode (sign-in, Issue 与 PR, feedback under the person's name) have their backend from here on
applyDevBackend(configureDevBackend)

// The light/dark control of the appearance page drives the same theme as the button in the bar, and the choice is kept
// where the bar reads it on the next start. A window that never opened the appearance page has no record and keeps the
// theme the bar stored, so starting the shell does not reset it.
registerModeHandler((mode) => {
  let saved = null
  try { saved = localStorage.getItem('foundation-ui.appearance') } catch { /* storage unavailable */ }
  if (!saved) return
  const theme = mode === 'system' ? 'auto' : mode
  setStoredTheme(theme)
  setTheme(theme)
})

import 'foundation-ui/all.css'
import './styles/m3e-web.less'

export async function initApp(app, config) {
    try {
        installSessionGuard()
        installExternalLinkHandler()
        const i18nInstance = await i18n()

        app.use(i18nInstance)
        app.provide('i18n', i18nInstance.global)
        setPlatformI18n(i18nInstance.global)
        const root = app.mount('#app')
        void import('./utils/appearanceBoot.js').then((boot) => boot.startAppearance())
        // First run: the guide opens by itself when the host has no setup and no paired client (never throws)
        void import('./utils/guideLauncher.js').then((launcher) => launcher.maybeAutoOpenGuide())
        config?.(app)
        // Let Vue finish the mount microtask before the GUI reveals the iframe.
        await Promise.resolve()
        return root
    } finally {
        markWebUiReady()
    }
}
