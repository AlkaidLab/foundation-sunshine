import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const root = new URL('..', import.meta.url)
const read = (rel) => readFileSync(new URL(rel, root), 'utf8')
const CODES = ['en','zh','zh_TW','en_GB','en_US','de','fr','es','it','ja','ko','ru','pt_BR','pt','bg','cs','pl','sv','tr','uk']
const NAMES = {
  en: 'English', en_GB: 'English (UK)', en_US: 'English (US)', zh: '简体中文', zh_TW: '繁體中文',
  de: 'Deutsch', fr: 'Français', es: 'Español', it: 'Italiano', ja: '日本語', ko: '한국어',
  ru: 'Русский', uk: 'Українська', pt: 'Português', pt_BR: 'Português (Brasil)', pl: 'Polski',
  sv: 'Svenska', tr: 'Türkçe', cs: 'Čeština', bg: 'Български',
}

test('welcome and settings offer all 20 languages by their own names, and zh_TW falls back through zh', () => {
  const welcome = read('views/Welcome.vue')
  const general = read('configs/tabs/General.vue')
  const i18n = read('config/i18n.js')
  for (const code of CODES) {
    assert.match(welcome, new RegExp("value: '" + code + "'"), 'welcome ' + code)
    assert.ok(welcome.includes(NAMES[code]), 'welcome name ' + code)
    assert.match(general, new RegExp("'" + code + "'"), 'settings ' + code)
    assert.ok(general.includes(NAMES[code]), 'settings name ' + code)
    assert.match(i18n, new RegExp("'" + code + "'"), 'supported ' + code)
  }
  assert.match(i18n, /zh_TW: \['zh', 'en'\]/)
  assert.match(general, /followSystem/)
})
