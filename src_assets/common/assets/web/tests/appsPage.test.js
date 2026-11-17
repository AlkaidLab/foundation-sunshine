import { readFileSync } from 'node:fs'
import assert from 'node:assert/strict'
import test from 'node:test'
import { appTags, envExampleFor, moveApp } from '../utils/appsPage.js'

const t = (key) => key
const m3eStyles = readFileSync(new URL('../styles/m3e-web.less', import.meta.url), 'utf8')

test('card marks follow the application settings in a fixed order', () => {
  assert.deepEqual(appTags({}, t), [])
  const tags = appTags({ 'exclude-global-prep-cmd': 'true', 'menu-cmd': [{}, {}], elevated: true, 'auto-detach': true, gamepad: 'ds5' }, t)
  assert.deepEqual(tags.map((tag) => tag.key), ['exclude-global-prep-cmd', 'menu-cmd', 'elevated', 'gamepad'])
  assert.equal(appTags({ gamepad: 'nonsense' }, t).length, 0)
  assert.equal(tags[1].count, 2)
})

test('moving an application keeps every other one in place', () => {
  const entries = ['a', 'b', 'c', 'd'].map((id) => ({ id }))
  const ids = (list) => list.map((e) => e.id).join('')
  const key = (e) => e.id
  assert.equal(ids(moveApp(entries, key, 'a', 'c')), 'bcad')
  assert.equal(ids(moveApp(entries, key, 'd', 'a')), 'dabc')
  assert.equal(ids(moveApp(entries, key, 'b', 'b')), 'abcd')
  assert.equal(ids(moveApp(entries, key, 'x', 'a')), 'abcd')
  assert.equal(ids(entries), 'abcd')
})

test('the environment example exists for the three platforms only', () => {
  for (const platform of ['windows', 'linux', 'macos']) assert.ok(envExampleFor(platform, t).code.includes('SUNSHINE_CLIENT_WIDTH'))
  assert.equal(envExampleFor('', t), null)
})

test('application cards avoid live backdrop filters in the embedded page', () => {
  const appsTag = m3eStyles.match(/\.apps-tag \{[^}]+\}/)?.[0] || ''
  assert.doesNotMatch(appsTag, /backdrop-filter/)
  assert.match(m3eStyles, /\.apps-page \.m-cover__check \{[^}]*backdrop-filter: none;/)
})
