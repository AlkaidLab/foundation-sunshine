// SPDX-License-Identifier: GPL-3.0-only
// 恢复默认设置 in the Web UI: where a person finds it (the search, the bottom of the settings, 日志与诊断, the system rail) and how the page talks to the host.
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX } = await import('../config/settingsData/index.js')
const { buildIndex, search } = await import('foundation-ui/search/index.js')
const { settingDocs } = await import('foundation-ui/search/docs.js')
const { actionDocs, searchTexts } = await import('foundation-ui/search/texts.js')
const { buildSettingsEntries } = await import('foundation-ui/settings/search.js')
const { labelOf } = await import('foundation-ui/settings/provider.js')
const { resetTexts, RESET_SECONDS } = await import('foundation-ui/reset')
const { diagnosticsTexts } = await import('foundation-ui/diagnostics')

const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8')
const shell = read('views/WebShell.vue')
const page = read('views/Reset.vue')
const general = read('configs/tabs/General.vue')
const troubleshooting = read('views/Troubleshooting.vue')

const entries = (locale) => buildSettingsEntries({ index: SEARCH_INDEX, label: (k) => labelOf(locale, k), altLabel: (k) => labelOf(locale === 'zh' ? 'en' : 'zh', k) })
const docsOf = (locale) => [...settingDocs(entries(locale)), ...actionDocs(locale, { resetSettings() {}, restart() {}, pair() {}, logs() {}, addApp() {} }, searchTexts(locale).types.action)]

test('the search finds it among every real setting, in both languages, by the words people type', () => {
  for (const locale of ['zh', 'en']) {
    const index = buildIndex(docsOf(locale))
    for (const query of ['reset', 'restore defaults', 'default settings', 'factory', 'factory reset', '恢复默认', '重置设置', '初始化', 'reset settings']) {
      const result = search(index, query, { perGroup: 1e6 })
      const actions = result.groups.find((g) => g.type === 'action')
      assert.ok(actions?.items.some((i) => i.doc.id === 'action:resetSettings'), `${locale}: “${query}” finds the action`)
    }
    const top = search(index, locale === 'zh' ? '恢复默认设置' : 'restore default settings').flat[0]
    assert.equal(top.id, 'action:resetSettings', `${locale}: its own title ranks it first`)
  }
})

test('the shell registers the page in the system rail, opens it from the search, and does not list it twice', () => {
  assert.match(shell, /reset: lazy\(\(\) => import\('\.\/Reset\.vue'\)\)/)
  assert.match(shell, /reset: \{ rail: 'system', path: null, hash: 'reset'[^}]*text: \(locale\) => resetTexts\(locale\)\.title \}/)
  assert.match(shell, /resetSettings: \(\) => jumpNav\.go\('reset'\)/)
  assert.match(shell, /id !== 'reset'/, 'the page is not a second search result of the same name')
  // the link a person copies is the address with the page's hash (a page without a path of its own had `null` in it)
  assert.match(shell, /page\.hash \? `\$\{normalise\(location\.pathname\)\}#\$\{page\.hash\}` : page\.path/)
})

test('the bottom of the settings and 日志与诊断 both lead to it', () => {
  assert.match(general, /<MResetEntry :locale="String\(locale\)" @open="openReset" \/>/)
  assert.match(general, /type: 'foundation-open', page: 'reset'/, 'inside the control panel the panel opens it (its page adds the panel preferences)')
  assert.match(general, /shell\?\.nav\?\.go\('reset'\)/)
  assert.match(troubleshooting, /resetSettings: shell\?\.nav \? \(\) => shell\.nav\.go\('reset'\) : undefined/)
  assert.equal(diagnosticsTexts('zh').resetSettings, resetTexts('zh').hostRow)
})

test('the page asks the host through apiFetch (session and CSRF token), for the host only', () => {
  assert.match(page, /createResetClient\(\(path, init\) => apiFetch\(path, init\)\)/)
  assert.match(page, /<MResetSettings[^>]*:host="host"[^>]*:panel="null"/)
  assert.match(page, /\/api\/runtime\/sessions/)
})

test('the confirm button counts down five seconds, in both languages', () => {
  assert.equal(RESET_SECONDS, 5)
  for (const locale of ['zh', 'en']) assert.match(resetTexts(locale).seconds, /\{n\}/)
})
