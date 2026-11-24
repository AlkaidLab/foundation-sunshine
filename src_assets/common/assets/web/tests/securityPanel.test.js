import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// 主机设置 › 安全与登录: a stale browser session can say signed out in the control panel too. The way back is the normal sign-in form, then this page again.
const read = (p) => fs.readFileSync(new URL(`../${p}`, import.meta.url), 'utf8')
const host = fs.readFileSync(new URL('../../../../../src/confighttp/auth.cpp', import.meta.url), 'utf8')

test('a signed-out panel re-enters the account and returns to the security page', () => {
  const view = read('views/SecurityLogin.vue')
  assert.match(view, /const toLogin = \(\) => window\.location\.assign\(loginUrl\('\/password'\)\)/)
  assert.doesNotMatch(view, /inPanel|password\/reset|apiPostJson\('\/api\/password'/)
})

test('the host says the panel is signed in and local: its session answer honours the local credential, and the password route renames the account', () => {
  // GET /api/auth/session: the local token (loopback, no forwarding header) is authenticated and local, and the account is named
  assert.match(host, /local_auth_token::matches\(\*local_token\.value\) && credentials_exist\(\)\) \{[\s\S]{0,400}body\["authenticated"\] = true;\s*body\["local"\] = true;\s*body\["username"\] = snapshot\.username;/)
  // POST /api/auth/password: the current password is verified in every case; a new user name is optional and keeps the password when no new one is given
  assert.match(host, /const bool renames = has_new_username && !new_username\.empty\(\) && new_username != snapshot\.username;/)
  assert.match(host, /save_credentials_locked\(renames \? new_username : snapshot\.username, new_password\)/)
  assert.match(host, /if \(!verify_credentials\(snapshot\.username, current_password\)\)/)
})
