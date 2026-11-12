import { CSRF_HEADER, loginUrl, readCsrfToken } from 'foundation-ui/auth'

// The Web UI's side of the host's session sign-in (docs/api/auth.md of the host), installed once in front of `fetch`:
//   - a state-changing request to this host's /api/ carries the CSRF token the host set (the host rejects it without), whoever makes it. The cookie is named after the host's service port
//     (`__Host-sunshine_csrf_<port>` unless the port is the default), so it is found by the page's own port; the token of the last sign-in, session or password answer of the host is the
//     fallback when the cookie cannot be read, and it is replaced by every such answer, so a credential change never leaves the page with the token of the session it replaced;
//   - a 401 from /api/ (the session ended, or there never was one) sends the page to /login with the path to come back to. The sign-in page itself and /api/auth/ calls are never redirected,
//     so a wrong password or the login page's own probes cannot loop. A host on its own computer (loopback) never answers 401, so the panel's embedded page is unaffected.
const SAFE_METHODS = new Set(['GET', 'HEAD', 'OPTIONS'])
// the answers of the host that carry the token of the session they describe
const TOKEN_ANSWERS = /^\/api\/auth\/(session|login|setup|password)\/?$/

export function installSessionGuard(win = globalThis.window) {
  if (!win || typeof win.fetch !== 'function' || win.__sessionGuard) return false
  const native = win.fetch.bind(win)
  let leaving = false
  let answered = ''
  win.fetch = async (input, init) => {
    const request = typeof input === 'object' && input !== null && 'url' in input ? input : null
    let url
    try { url = new URL(request ? request.url : String(input), win.location.href) } catch { return native(input, init) }
    const own = url.origin === win.location.origin && url.pathname.startsWith('/api/')
    let next = init
    if (own) {
      const method = String(init?.method || request?.method || 'GET').toUpperCase()
      const token = SAFE_METHODS.has(method) ? '' : readCsrfToken(win.document?.cookie, win.location.port) || answered
      if (token) {
        const headers = new Headers(init?.headers || request?.headers || {})
        if (!headers.has(CSRF_HEADER)) headers.set(CSRF_HEADER, token)
        next = { ...init, headers }
      }
    }
    const response = await native(input, next)
    if (own && TOKEN_ANSWERS.test(url.pathname) && response.ok && typeof response.clone === 'function') {
      response.clone().json().then((body) => { if (typeof body?.csrf_token === 'string' && body.csrf_token) answered = body.csrf_token }).catch(() => {})
    }
    if (own && response.status === 401 && !url.pathname.startsWith('/api/auth/') && win.location.pathname !== '/login' && !leaving) {
      leaving = true
      win.location.assign(loginUrl(`${win.location.pathname}${win.location.search}`))
    }
    return response
  }
  win.__sessionGuard = true
  return true
}
