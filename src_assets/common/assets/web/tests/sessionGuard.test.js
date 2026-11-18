import test from 'node:test'
import assert from 'node:assert/strict'
import { installSessionGuard } from '../utils/sessionGuard.js'

function fakeWindow({ path = '/config', search = '?tab=network', cookie = '', answer = { status: 200 } } = {}) {
  const sent = []
  const win = {
    location: { origin: 'https://host:57990', port: '57990', href: `https://host:57990${path}${search}`, pathname: path, search, assigned: [], assign(u) { this.assigned.push(u) } },
    document: { cookie },
    fetch: async (input, init) => { sent.push({ input, init }); return { status: typeof answer === 'function' ? answer(String(input)).status : answer.status } },
  }
  return { win, sent }
}
test('a state-changing request to this host carries the CSRF token; a read and a foreign host do not', async () => {
  const { win, sent } = fakeWindow({ cookie: 'a=1; __Host-sunshine_csrf=TOK' })
  assert.equal(installSessionGuard(win), true); assert.equal(installSessionGuard(win), false, 'once')
  await win.fetch('/api/config', { method: 'POST', body: '{}' })
  assert.equal(sent[0].init.headers.get('X-CSRF-Token'), 'TOK')
  await win.fetch('/api/config'); assert.equal(sent[1].init, undefined)
  await win.fetch('https://api.github.com/x', { method: 'POST' }); assert.equal(sent[2].init.headers, undefined)
  await win.fetch('/api/apps', { method: 'DELETE', headers: { 'X-CSRF-Token': 'own' } }); assert.equal(sent[3].init.headers.get('X-CSRF-Token'), 'own')
  await win.fetch('/static.js', { method: 'POST' }); assert.equal(sent[4].init.headers, undefined, 'only /api/')
})
test('without the cookie nothing is added', async () => {
  const { win, sent } = fakeWindow(); installSessionGuard(win)
  await win.fetch('/api/config', { method: 'POST' }); assert.equal(sent[0].init.headers, undefined)
})
test('a 401 from the API sends the page to the sign-in with the way back, once; the sign-in itself never loops', async () => {
  const a = fakeWindow({ answer: { status: 401 } }); installSessionGuard(a.win)
  await a.win.fetch('/api/apps'); await a.win.fetch('/api/config')
  assert.deepEqual(a.win.location.assigned, ['/login?next=%2Fconfig%3Ftab%3Dnetwork'])
  const b = fakeWindow({ answer: { status: 401 } }); installSessionGuard(b.win)
  await b.win.fetch('/api/auth/login', { method: 'POST' }); await b.win.fetch('/api/auth/session'); assert.deepEqual(b.win.location.assigned, [])
  const c = fakeWindow({ path: '/login', search: '?next=%2F', answer: { status: 401 } }); installSessionGuard(c.win)
  await c.win.fetch('/api/config'); assert.deepEqual(c.win.location.assigned, [])
  const d = fakeWindow({ answer: { status: 200 } }); installSessionGuard(d.win); await d.win.fetch('/api/apps'); assert.deepEqual(d.win.location.assigned, [])
})

test('a host on another service port names its cookie after the port; the page finds it by its own port (the 403 after the guide)', async () => {
  // a host on custom ports: service port 57989, Web UI 57990, cookie __Host-sunshine_csrf_57989; the page used to read the fixed name and sent no header
  const { win, sent } = fakeWindow({ cookie: 'a=1; __Host-sunshine_session_57989=x; __Host-sunshine_csrf_57989=PORTTOK; __Host-sunshine_csrf_47989=OTHER' })
  installSessionGuard(win)
  await win.fetch('/api/perf/frame-rates', { method: 'POST', body: '{}' })
  assert.equal(sent[0].init.headers.get('X-CSRF-Token'), 'PORTTOK')
  // the default service port keeps the unsuffixed name; another host's cookie on the same name is not taken for ours
  const d = fakeWindow({ cookie: '__Host-sunshine_csrf=DEF; __Host-sunshine_csrf_57989=ELSE' }); d.win.location.port = '47990'; installSessionGuard(d.win)
  await d.win.fetch('/api/config', { method: 'POST' }); assert.equal(d.sent[0].init.headers.get('X-CSRF-Token'), 'DEF')
  const none = fakeWindow({ cookie: '__Host-sunshine_csrf_1111=A; __Host-sunshine_csrf_2222=B' }); installSessionGuard(none.win)
  await none.win.fetch('/api/config', { method: 'POST' }); assert.equal(none.sent[0].init.headers, undefined, 'two foreign cookies and none of ours: no guess')
})

test('the token of the last sign-in, session or password answer is the fallback when the cookie cannot be read, and the newest answer wins', async () => {
  const tokens = ['FIRST', 'SECOND']
  const sent = []
  const win = {
    location: { origin: 'https://host:57990', href: 'https://host:57990/', pathname: '/', search: '', port: '57990', assign() {} },
    document: { cookie: '' },
    fetch: async (input, init) => { sent.push({ input: String(input), init }); const auth = String(input).startsWith('/api/auth/'); return { ok: true, status: 200, clone() { return { json: async () => (auth ? { csrf_token: tokens.shift() } : {}) } } } },
  }
  installSessionGuard(win)
  await win.fetch('/api/auth/login', { method: 'POST', body: '{}' })
  await new Promise((r) => setTimeout(r, 0))
  await win.fetch('/api/config', { method: 'POST', body: '{}' })
  assert.equal(sent[1].init.headers.get('X-CSRF-Token'), 'FIRST')
  await win.fetch('/api/auth/password', { method: 'POST', body: '{}' })
  await new Promise((r) => setTimeout(r, 0))
  await win.fetch('/api/config', { method: 'POST', body: '{}' })
  assert.equal(sent[3].init.headers.get('X-CSRF-Token'), 'SECOND', 'a credential change replaces the token')
  win.document.cookie = '__Host-sunshine_csrf_57989=COOKIE'
  await win.fetch('/api/config', { method: 'POST', body: '{}' })
  assert.equal(sent[4].init.headers.get('X-CSRF-Token'), 'COOKIE', 'the cookie wins when the page can read it')
})
