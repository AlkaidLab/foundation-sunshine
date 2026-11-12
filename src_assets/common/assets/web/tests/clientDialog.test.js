import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { reactive, ref } from 'vue'
import { CLIENTS, clientsForOs } from 'foundation-ui/clients'
import { extraLinks, fileGroups, targetName, useClientDialog } from '../utils/clientDialog.js'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')
const asset = (type, size = 16e6) => ({ type, name: `${type}.bin`, size, sha256: 'a'.repeat(64), url: `https://cnb.example/${type}`, fallbackUrl: `https://github.example/${type}` })
const MAC_ASSETS = [asset('macos-arm64-dmg'), asset('macos-universal-dmg'), asset('macos-x64-dmg'), { type: 'macos-universal-dmg-zsync', url: 'x' }]
const enhanced = clientsForOs('macos').find((x) => x.client.id === 'macos-enhanced')
const card = { key: 'macos-enhanced:macos', client: enhanced.client, target: enhanced.target }

const model = (state = {}) => {
  const data = reactive({ assets: [], loaded: false, ...state })
  const focused = []
  const dialog = useClientDialog({ assetsOf: () => data.assets, loadedOf: () => data.loaded, locale: ref('en'), restoreFocus: (key) => focused.push(key) })
  return { data, focused, ...dialog }
}

test('no file is listed before a click: the popup has no card and no groups', () => {
  const m = model({ assets: MAC_ASSETS, loaded: true })
  assert.equal(m.card.value, null)
  assert.deepEqual(m.groups.value, [])
})

test('a click opens that client\'s files: best first, the recommended one marked, the files that are not for people left out', () => {
  const m = model({ assets: MAC_ASSETS, loaded: true })
  m.open(card)
  assert.equal(m.card.value.client.id, 'macos-enhanced')
  assert.equal(m.groups.value.length, 1)
  const [group] = m.groups.value
  assert.equal(group.state, 'ready')
  assert.deepEqual(group.rows.map((r) => r.type), ['macos-universal-dmg', 'macos-arm64-dmg', 'macos-x64-dmg'])
  assert.deepEqual(group.rows.map((r) => r.recommended), [true, false, false])
  const first = group.rows[0]
  assert.ok(first.sizeLabel && first.sha256 && first.githubUrl && first.cnbUrl, 'size, hash and both hosts (the GitHub fallback) are there')
})

test('Escape (and the close button, and the backdrop) end in close(): the files are gone and the card that opened it gets the focus back', () => {
  const m = model({ assets: MAC_ASSETS, loaded: true })
  m.open(card)
  m.close()
  assert.equal(m.card.value, null)
  assert.deepEqual(m.groups.value, [])
  assert.deepEqual(m.focused, ['macos-enhanced:macos'])
  m.close() // closing a closed popup gives the focus to nobody
  assert.deepEqual(m.focused, ['macos-enhanced:macos'])
})

test('files that arrive late fill the open popup in place: skeleton while reading, rows when read, the links when nothing could be read', () => {
  const m = model()
  m.open(card)
  assert.equal(m.groups.value[0].state, 'loading')
  assert.equal(m.card.value.key, card.key, 'the same popup, not a new one')
  m.data.assets = MAC_ASSETS
  m.data.loaded = true
  assert.equal(m.groups.value[0].state, 'ready')
  assert.equal(m.groups.value[0].rows.length, 3)
  const failed = model({ loaded: true })
  failed.open(card)
  assert.equal(failed.groups.value[0].state, 'failed')
})

test('a client with several systems lists each system once; a client without release data (a store, a partner\'s page) has no files', () => {
  const desktop = CLIENTS.find((c) => c.id === 'vplus-desktop')
  const groups = fileGroups({ client: desktop, target: null }, { assets: [asset('windows-x64-installer'), asset('macos-universal-dmg'), asset('linux-x64-appimage'), asset('steam-link-zip')], loaded: true })
  assert.deepEqual(groups.map((g) => g.os), ['windows', 'macos', 'linux', 'tv'])
  assert.equal(groups[3].name, 'TV · Steam Deck')
  const android = CLIENTS.find((c) => c.id === 'vplus-android')
  assert.equal(fileGroups({ client: android, target: null }, { assets: [asset('android-apk')], loaded: true }).length, 1, 'Android, Android TV and Quest take the one Android file')
  for (const id of ['vplus-harmony', 'voidlink', 'crown', 'baozi']) assert.deepEqual(fileGroups({ client: CLIENTS.find((c) => c.id === id), target: null }, { assets: [], loaded: true }), [], id)
  assert.equal(targetName({ os: 'android', platform: 'quest' }, 'en'), 'Android · Quest')
})

test('the stores and pages of a card are listed once each, from its platforms and its partner card', () => {
  const links = extraLinks({ client: { targets: [{ platform: 'a' }, { platform: 'b' }] }, target: null }, { platformOf: (t) => ({ a: { links: [{ url: 'u1' }, { url: 'u2' }] }, b: { links: [{ url: 'u2' }] } })[t.platform], partnerOf: (t) => (t.platform === 'b' ? { links: [{ url: 'u3' }] } : undefined) })
  assert.deepEqual(links.map((l) => l.url), ['u1', 'u2', 'u3'])
})

test('the page: the files live in the popup only, a card is a real button that opens it, the dialog is the shared MDialog', () => {
  const page = read('../views/Downloads.vue')
  const template = page.slice(page.indexOf('<template>'))
  const dialogAt = template.indexOf('<MDialog')
  assert.ok(dialogAt > 0 && template.indexOf('<MClientAssets') > dialogAt, 'MClientAssets is inside the popup, not above the cards')
  assert.equal(template.split('<MClientAssets').length - 1, 1)
  assert.match(template, /<MDialog [^>]*:model-value="!!dialogCard"[^>]*@update:model-value="\(open\) => !open && closeDialog\(\)"/)
  assert.match(template, /<div v-if="dialogCard" class="dl-dialog__body">/, 'nothing of the popup is rendered without a card')
  assert.match(template, /<button type="button" class="dl-card__main" :data-card="card\.key" aria-haspopup="dialog" @click="openDialog\(card\)">/)
  assert.ok(!/\blead(Rows|Version|tag)\b|const lead\b|v-if="lead\b|dl-leadtag/.test(page), 'the rows above the cards (the lead block) are gone')
  assert.ok(!/<MSheet/.test(page), 'no side sheet opens by itself any more')
  assert.match(page, /import \{[^}]*\bMDialog\b[^}]*\} from 'foundation-ui\/vue'/)
})

test('MDialog closes on Escape: the native cancel event ends in open = false, and it hands the focus back to the element that opened it', () => {
  const dialog = read('../../../../../../ds5-ui-m3e/src/vue/MDialog.vue')
  assert.match(dialog, /@cancel="onCancel"/)
  assert.match(dialog, /const onCancel = \(event\) => \{\s*event\.preventDefault\(\)\s*open\.value = false/)
  assert.match(dialog, /if \(opener instanceof HTMLElement\) opener\.focus\(\)/)
})

test('the loading and failure texts exist in every language of the page', () => {
  const dir = new URL('../i18n/downloads/', import.meta.url)
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.json'))
  assert.ok(files.length >= 20)
  for (const f of files) assert.ok(JSON.parse(fs.readFileSync(new URL(f, dir), 'utf8')).ui.filesFailed, f)
})
