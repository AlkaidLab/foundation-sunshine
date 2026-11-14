import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// 主机设置 › 安全与登录 in the control panel: no 「已退出登录」, no 登录 button; the account and a form that asks for the current password. A browser keeps its sign-in.
const read = (p) => fs.readFileSync(new URL(`../${p}`, import.meta.url), 'utf8')
const host = fs.readFileSync(new URL('../../../../../src/confighttp/auth.cpp', import.meta.url), 'utf8')

test('in the panel the page has no sign-in handler; a browser has one', () => {
  const view = read('views/SecurityLogin.vue')
  assert.match(view, /const inPanel = window\.isTauri === true && window\.parent !== window/)
  assert.match(view, /const toLogin = inPanel \? null : \(\) => window\.location\.assign\(loginUrl\('\/'\)\)/)
})

test('the host says the panel is signed in and local: its session answer honours the local credential, and the password route renames the account', () => {
  // GET /api/auth/session: the local token (loopback, no forwarding header) is authenticated and local, and the account is named
  assert.match(host, /local_auth_token::matches\(\*local_token\.value\) && credentials_exist\(\)\) \{[\s\S]{0,400}body\["authenticated"\] = true;\s*body\["local"\] = true;\s*body\["username"\] = snapshot\.username;/)
  assert.match(host, /const bool forwarded = request->header\.find\("x-forwarded-for"\)/)
  // POST /api/auth/password: the current password is verified in every case; a new user name is optional and keeps the password when no new one is given
  assert.match(host, /const bool renames = has_new_username && !new_username\.empty\(\) && new_username != snapshot\.username;/)
  assert.match(host, /save_credentials_locked\(renames \? new_username : snapshot\.username, new_password\)/)
  assert.match(host, /if \(!verify_credentials\(snapshot\.username, current_password\)\)/)
})
