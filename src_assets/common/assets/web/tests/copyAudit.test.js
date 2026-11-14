import test from 'node:test'
import assert from 'node:assert/strict'
import { readdirSync, readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

// The copy audit: a supporting line must carry information its title does not. These lines only restated their title, explained
// the obvious or talked about the interface itself, so they were removed from every language file; they must not come back.
const web = join(dirname(fileURLToPath(import.meta.url)), '..')
const locales = join(web, 'public/assets/locale')

const REMOVED_KEYS = [
  'apps.scan_options.ai_desc', 'apps.scan_options.directory_desc', 'apps.no_apps_desc', 'apps.cover_search_empty_desc',
  'apps.scan_result_try_different_keywords', 'apps.app_name_desc', 'pin.remove_paired_devices_desc', 'pin.qr_pairing_desc',
  'troubleshooting.reopen_setup_wizard_desc', 'troubleshooting.logout_desc', 'resource_card.join_group_desc',
  'resource_card.open_source_desc', 'resource_card.tutorial_desc', 'resource_card.third_party_desc', 'home.promo_downloads',
  'home.promo_sponsor', 'notifications.webhook.description', 'ds5.pending.hint', 'apps.cover_search_hint',
]
const has = (object, key) => key.split('.').reduce((node, part) => (node && typeof node === 'object' ? node[part] : undefined), object) !== undefined

test('lines that only restated their title are gone from every language file', () => {
  for (const file of readdirSync(locales).filter((name) => name.endsWith('.json'))) {
    const messages = JSON.parse(readFileSync(join(locales, file), 'utf8'))
    for (const key of REMOVED_KEYS) assert.ok(!has(messages, key), `${file} brought back ${key}`)
  }
})

test('the removed lines are not read by any template', () => {
  const sources = []
  const walk = (dir) => { for (const entry of readdirSync(dir, { withFileTypes: true })) { const path = join(dir, entry.name); if (entry.isDirectory()) { if (!['tests', 'public', 'node_modules'].includes(entry.name)) walk(path) } else if (/\.(vue|js)$/.test(entry.name)) sources.push(readFileSync(path, 'utf8')) } }
  walk(web)
  const all = sources.join('\n')
  for (const key of REMOVED_KEYS) assert.ok(!all.includes(`'${key}'`) && !all.includes(`"${key}"`), `${key} is read again`)
})

test('the sentences marked as filler stay out of the English and Chinese files', () => {
  const banned = ['作为扫描后的增强步骤执行', 'Runs after the scan to enhance the results', '到哪里查找和下载新版本', '你正在主机本机上']
  for (const file of ['zh.json', 'en.json']) {
    const text = readFileSync(join(locales, file), 'utf8')
    for (const phrase of banned) assert.ok(!text.includes(phrase), `${file}: ${phrase}`)
  }
})
