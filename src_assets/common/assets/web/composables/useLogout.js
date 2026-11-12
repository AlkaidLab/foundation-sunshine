import { createAuthClient, fetchRequest, loginUrl } from 'foundation-ui/auth'

/**
 * Logout composable.
 * A host with sessions (docs/api/auth.md of the host): a signed-in remote browser ends its session on the host and goes to the sign-in page; the host's own computer (loopback) has no
 * session, so it runs `onLocalhost`. A host without the session API keeps the older sign-out below.
 */
export function useLogout() {
  const legacy = (opts) => {
    const xhr = new XMLHttpRequest()
    xhr.open('GET', '/api/logout?t=' + Date.now(), true)
    xhr.setRequestHeader('Authorization', 'Basic ' + btoa('logout:logout'))
    // 200 ms later the page is on the home page whatever the answer was; only an explicit 200 from the host's own computer cancels that
    const watchdog = setTimeout(() => {
      try { xhr.abort() } catch { /* already finished */ }
      window.location.href = '/'
    }, 200)
    xhr.onreadystatechange = () => {
      if (xhr.readyState === 4 && xhr.status === 200) {
        clearTimeout(watchdog)
        opts.onLocalhost?.()
      }
    }
    xhr.send()
  }
  /**
   * @param { { onLocalhost?: () => void } } [opts]
   */
  const logout = async (opts = {}) => {
    const auth = createAuthClient({ request: fetchRequest() })
    const state = await auth.session()
    if (!state.ok) return legacy(opts)
    if (state.data.local && !state.data.sessionId) return opts.onLocalhost?.()
    const done = await auth.logout()
    if (done.ok || done.error.kind === 'signed-out') window.location.href = loginUrl('/')
    else window.location.href = '/'
  }
  return { logout }
}
