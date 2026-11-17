import './helpers/foundationUi.mjs'
import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const web = path.join(path.dirname(fileURLToPath(import.meta.url)), '..')
const read = (p) => fs.readFileSync(path.join(web, p), 'utf8')
const page = read('views/Pin.vue')
const locales = Object.fromEntries(fs.readdirSync(path.join(web, 'public/assets/locale')).filter((f) => f.endsWith('.json')).map((f) => [f.replace(/\.json$/, ''), JSON.parse(read(`public/assets/locale/${f}`))]))

const NEW_KEYS = ['stopped', 'restore', 'unpair_named', 'sheet_stopped', 'disable_row', 'disable_action', 'disable_row_text', 'unpair', 'unpair_row_text', 'disable_title', 'disable_body', 'disable_how', 'unpair_title', 'unpair_body', 'unpair_all_title', 'unpair_all_body', 'restored', 'stopped_note']
const OBSOLETE_KEYS = ['disabled', 'allow_device', 'blocked', 'allowed', 'disable_disconnect', 'disable_note', 'unpair_device', 'confirm_unpair']

test('a device row has no switch: cancel pairing is a red icon button beside the gear, a stopped device says so and is restored from its row', () => {
  assert.doesNotMatch(page, /MSwitch|pin-row__switch|requestToggle/)
  assert.match(page, /<button type="button" class="pin-row__unpair"[^>]*:aria-label="\$t\('connections\.unpair_named', \{ name: client\.name \}\)"[^>]*:data-tip="\$t\('connections\.unpair_named', \{ name: client\.name \}\)"[^>]*@click="requestUnpair\(client\)"><MIcon name="link_off" \/><\/button>/)
  assert.match(page, /<i class="m-split-item__divider"[^>]*\/>\s*<button type="button" class="m-split-item__gear"/, 'the red button and the gear are two controls with a divider between them')
  assert.match(page, /v-if="client\.enabled === false" class="pin-row__stopped"[\s\S]*connections\.stopped[\s\S]*@click="restoreClient\(client\)"[\s\S]*connections\.restore/)
  const less = read('styles/m3e-web.less')
  assert.match(less, /\.pin-row__unpair \{[^}]*width: 44px; height: 44px;[^}]*color: var\(--md-sys-color-error\)/, 'a 44 px target in the error colour of the theme')
  assert.match(less, /\.pin-row__unpair:focus-visible \{ outline: 2px solid var\(--md-sys-color-error\)/)
})

test('the devices that cannot connect are one group of their own, below the others, under 「已停用」', () => {
  assert.match(page, /const listGroups = computed\(\(\) => \{\s*const on = groupClients\(shown\.value\.filter\(\(c\) => c\.enabled !== false\)\)\s*const off = shown\.value\.filter\(\(c\) => c\.enabled === false\)\s*return off\.length \? \[\.\.\.on, \{ kind: '__stopped', items: off \}\] : on/)
  assert.match(page, /group\.kind === '__stopped' \? \$t\('connections\.stopped'\)/)
})

test('disabling is in the danger zone of the device sheet, with cancel pairing; restoring a device asks nothing, disabling always asks', () => {
  const sheet = page.slice(page.indexOf('<MSheet :model-value="!!selectedClient"'), page.indexOf('<ConfirmDialog'))
  assert.match(sheet, /<section class="pin-danger" :aria-label="\$t\('connections\.danger_zone'\)">[\s\S]*connections\.disable_row[\s\S]*requestDisable\(selectedClient\)[\s\S]*connections\.unpair'\)[\s\S]*requestUnpair\(selectedClient\)/)
  assert.match(sheet, /<MBanner v-if="selectedClient\.enabled === false"[\s\S]*connections\.sheet_stopped[\s\S]*restoreClient\(selectedClient\)/, 'a stopped device offers 恢复 at the top of its sheet')
  assert.ok(sheet.indexOf('connections.sheet_stopped') < sheet.indexOf("connections.display_settings"), 'before the settings, where it is seen first')
  assert.match(page, /const requestDisable = \(client\) => \{\s*closeDevice\(\)\s*clientError\.value = ''\s*confirmAction\.value = \{ type: 'disable', client \}/)
  assert.match(page, /const restoreClient = async \(client\) => \{[\s\S]*setClientEnabled\(client\.uuid, true\)/)
  assert.doesNotMatch(page, /confirmAction\.value = \{ type: 'disable'[^}]*\}\s*\n?\s*return\s*\n\s*\}\s*\n\s*if \(await setClientEnabled\(client\.uuid, enabled\)/, 'the old rule (ask only while streaming) is gone')
})

test('three questions, each says what happens and what stays; the one for disabling says how to undo it; the one for all devices says how many', () => {
  const dialog = page.slice(page.indexOf('<ConfirmDialog'), page.indexOf('<MSnackbar'))
  assert.match(dialog, /connections\.disable_how/)
  assert.match(page, /t\('connections\.disable_title', \{ name: confirmAction\.value\.client\?\.name \}\)/)
  assert.match(page, /'connections\.unpair_all_body'[^\n]*\{ n: clients\.value\.length \}/)
  assert.match(dialog, /confirmAction\?\.type === 'unpair-all' \? \$t\('troubleshooting\.unpair_all'\) : \$t\('connections\.unpair'\)/, 'the buttons carry the verbs 停用 / 取消所有配对 / 取消配对')
  assert.match(page, /\{ id: 'toggle', icon: enabled \? 'block' : 'undo'/, 'the context menu says the same words')
})

test('every language has the words of the page and none of the old switch wording is left', () => {
  assert.equal(Object.keys(locales).length, 20)
  for (const [code, d] of Object.entries(locales)) {
    for (const k of NEW_KEYS) assert.ok(typeof d.connections[k] === 'string' && d.connections[k].length > 1, `${code} connections.${k}`)
    for (const k of OBSOLETE_KEYS) assert.equal(d.connections[k], undefined, `${code} connections.${k} is not used any more`)
    assert.equal(d.pin.unpair_all_confirm, undefined, `${code} pin.unpair_all_confirm`)
    for (const k of ['unpair_named', 'disable_title', 'unpair_title', 'restored', 'stopped_note']) assert.ok(d.connections[k].includes('{name}'), `${code} ${k} has {name}`)
    assert.ok(d.connections.unpair_all_body.includes('{n}'), `${code} unpair_all_body has {n}`)
  }
})

test('the words are 停用 / 恢复 / 取消配对 / 取消所有配对, the same in Simplified and Traditional Chinese', () => {
  const zh = locales.zh; const tw = locales.zh_TW
  assert.equal(zh.connections.stopped, '已停用'); assert.equal(zh.connections.restore, '恢复'); assert.equal(zh.connections.unpair, '取消配对'); assert.equal(zh.connections.disable_action, '停用')
  assert.equal(zh.troubleshooting.unpair_all, '取消所有配对'); assert.equal(tw.troubleshooting.unpair_all, '取消所有配對')
  assert.equal(tw.connections.stopped, '已停用'); assert.equal(tw.connections.restore, '恢復'); assert.equal(tw.connections.unpair, '取消配對')
  for (const d of [zh, tw]) for (const [k, v] of Object.entries(d.connections)) assert.doesNotMatch(v, /禁用|禁止连接|禁止連線|解除配对|解除配對/, `${k}: the old words are gone`)
  assert.equal(zh.connections.display_hint, '这些设置仅用于当前设备；停用时仍会保留。')
})

test('the pairing wait texts are translated in every language that has them', () => {
  for (const [code, d] of Object.entries(locales)) {
    if (code.startsWith('en')) continue
    assert.notEqual(d.connections.waiting_client, locales.en.connections.waiting_client, `${code} waiting_client is not English`)
    assert.notEqual(d.connections.pairing, locales.en.connections.pairing, `${code} pairing is not English`)
  }
})

test('cancel all pairings ends the sessions of every paired client before the pairing list is cleared, as unpairing one client does', () => {
  const host = fs.readFileSync(path.join(web, '../../../../src/confighttp/clients.cpp'), 'utf8')
  const body = host.slice(host.indexOf('unpairAll('), host.indexOf('unpair(resp_https_t'))
  const ends = body.indexOf('terminate_sessions_of_all(nvhttp::get_all_clients()')
  const erases = body.indexOf('nvhttp::erase_all_clients()')
  assert.ok(ends > 0 && erases > ends, 'the sessions are ended first, then the clients are erased')
  assert.match(body, /rtsp_stream::terminate_client_sessions\(uuid\)/)
})
