import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'
import { authTexts } from 'foundation-ui/auth'

const read = (f) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')

test('the sign-in page footer links 官网 and B 站 first (site by language, channel for every language), then the docs and the repository', () => {
  const src = read('views/Login.vue')
  assert.match(src, /\{ id: 'site', url: brandUrl\('site', lang\.value\) \}, \{ id: 'bilibili', url: BILIBILI\.url \}, \{ id: 'docs'.*\{ id: 'github'/)
  assert.equal(authTexts('zh').site, '官网')
  assert.equal(authTexts('zh').bilibili, 'B 站')
  assert.ok(authTexts('en').site && authTexts('en').bilibili)
})
