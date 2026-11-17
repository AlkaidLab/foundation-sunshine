import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'

const dir = new URL('../', import.meta.url)
const read = (f) => readFileSync(new URL(f, dir), 'utf8')
const m3e = (f) => readFileSync(new URL(`../../../../../node_modules/foundation-ui/src/${f}`, import.meta.url), 'utf8')

test('the sign-in page is the shared sign-in form, whose fields submit the form on Enter and keep the submit button', () => {
  assert.match(read('views/Login.vue'), /<MLoginPage /)
  const page = m3e('vue/MLoginPage.vue')
  assert.match(page, /<form [^>]*@submit\.prevent="submit" @keydown="submitOnEnter">/)
  assert.match(page, /<MButton type="submit" class="m-login__submit"/)
  const helper = m3e('vue/submitOnEnter.js')
  assert.match(helper, /form\.requestSubmit\(\)/)
  assert.match(helper, /isComposing/)
})

test('the Web UI keeps its own log out for a browser: the account menu asks, the shell confirms, the composable ends the session', () => {
  assert.match(read('components/layout/ShellUtils.vue'), /@click="shell\?\.askLogout\(\)"/)
  assert.match(read('views/WebShell.vue'), /askLogout: \(\) => \{ logoutAsk\.value = true \}/)
  assert.match(read('composables/useLogout.js'), /export function useLogout\(\)/)
})
