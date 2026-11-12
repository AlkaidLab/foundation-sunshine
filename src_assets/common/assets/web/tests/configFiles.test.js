import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { PATH_KEYS, configFileName, configLines, configText, differences, importPlan, parseConfigText } from '../utils/configBackup.js'

const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')
const read = (p) => readFileSync(join(WEB, p), 'utf8')

test('an export is the lines of sunshine.conf: the facts the host adds to its answer and every secret are left out', () => {
  const answer = { status: 'true', platform: 'windows', version: '2026.1101', port: 57989, upnp: 'enabled', sunshine_name: 'windows-test-host', display_devices: [{ device_id: 'x' }], adapters: [{ name: 'RTX' }], nvidia_vsync: { supported: true }, active_encoder: 'nvenc', pair_name: 'Sky', sunshine_password: 'hunter2', sunshine_salt: 'abc', widget_token_configured: true }
  const lines = configLines(answer)
  assert.deepEqual(Object.keys(lines), ['port', 'sunshine_name', 'upnp'])
  const text = configText(lines, { host: 'windows-test-host', at: new Date(2026, 10, 4, 14, 20) })
  assert.equal(text, '# Foundation Sunshine configuration of windows-test-host, exported 2026-11-04 14:20\nport = 57989\nsunshine_name = windows-test-host\nupnp = enabled\n')
  assert.deepEqual(parseConfigText(text), { port: '57989', sunshine_name: 'windows-test-host', upnp: 'enabled' }, 'what is exported reads back the same')
  assert.equal(configFileName('Sky PC / 2', new Date(2026, 10, 4)), 'sunshine-Sky-PC-2-20261104.conf')
})

test('an import is read like the host reads the file, and a file without setting lines is not a configuration', () => {
  assert.deepEqual(parseConfigText('# c\n\n port = 47990 \nweird line\nexternal_endpoints = a.example.net, [2001:db8::1]:57989\n=x\n'), { port: '47990', external_endpoints: 'a.example.net, [2001:db8::1]:57989' })
  assert.equal(parseConfigText('hello\nworld'), null)
  assert.equal(parseConfigText(''), null)
})

test('an import changes only settings the page can take as one value; file places, secrets and unknown keys are listed, not taken', () => {
  const plan = importPlan({ port: '47990', upnp: 'disabled', file_apps: 'D:/apps.json', sunshine_password: 'x', not_a_setting: '1', fps: '[60]' }, { port: 47989, upnp: 'disabled' })
  assert.deepEqual(plan.changes, [{ key: 'port', from: '47989', to: '47990' }])
  assert.equal(plan.same, 1)
  assert.deepEqual(plan.paths, ['file_apps'])
  assert.ok(plan.other.includes('sunshine_password') && plan.other.includes('not_a_setting'))
  assert.deepEqual([...PATH_KEYS].sort(), ['cert', 'credentials_file', 'file_apps', 'file_state', 'log_path', 'pkey'])
})

test('the differences from the defaults name the place of each setting, in the order of the pages', () => {
  const rows = differences({ port: 57989, upnp: 'enabled', min_log_level: 'info', locale: 'zh' })
  const port = rows.find((r) => r.key === 'port')
  assert.ok(port, 'a changed port is a difference')
  assert.equal(port.place.tab, 'network')
  assert.equal(port.place.focus, 'port')
  assert.equal(port.value, '57989')
  assert.ok(!rows.some((r) => r.key === 'min_log_level' && r.value === 'info' && r.def === 'info'), 'a value equal to its default is not a difference')
  assert.deepEqual(differences({}), [])
})

test('the page is a set of groups, not one closed accordion; every file row keeps its key, and import goes through the unsaved changes', () => {
  const page = read('configs/tabs/Files.vue')
  assert.ok(!/aria-expanded/.test(page), 'no group that is closed until it is needed')
  for (const k of PATH_KEYS) assert.match(page, new RegExp(`data-cfg="${k}"`))
  assert.match(page, /props\.config\[c\.key\] = c\.to/, 'an import writes into the configuration of the page (the unsaved changes)')
  assert.ok(!/apiFetch\([^)]*'POST'|method: 'POST'/.test(page), 'the page never saves on its own')
  assert.match(page, /save_text_file/, 'the control panel asks where to save the export')
})

test('cancelling the configuration export save dialog asks nothing again and shows no error', () => {
  const page = read('configs/tabs/Files.vue')
  assert.match(page, /saveThroughDialog\(invoke, 'save_text_file'/)
  assert.match(page, /if \(saved === null\) return/)
  assert.doesNotMatch(page, /if \(e === 'cancelled'\) return/)
})

test('the network page lists the paths and the endpoints with the calm overview, the local network included', () => {
  const page = read('configs/tabs/Network.vue')
  assert.match(page, /<MReachOverview [^>]*data-cfg="external_endpoints"/)
  assert.match(page, /:lan="lan"/)
  assert.ok(!/<MReachCard /.test(page))
})

test('the credentials row says its default is the state file itself (src/config.cpp sets credentials_file from file_state), not a second copy of the name', () => {
  const host = read('../../../../src/config.cpp')
  assert.match(host, /config::sunshine\.credentials_file = config::nvhttp\.file_state;\s*\n\s*path_f\(vars, "credentials_file"/)
  const page = read('configs/tabs/Files.vue')
  assert.match(page, /credentials_file: \{[^}]*sameAsState: true/)
  for (const loc of ['en', 'zh']) assert.ok(JSON.parse(read(`public/assets/locale/${loc}.json`)).config.path_same_as_state, `${loc} has the text`)
})
