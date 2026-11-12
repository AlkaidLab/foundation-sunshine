import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { SEARCH_INDEX, CONSTRAINTS, SETTING_META, validateSetting } from 'foundation-ui/settings/index.js'
import { tokenize } from '../../../../../scripts/gen-config-index.mjs'

// What the host takes for a setting is in src/config.cpp. This walks the host's ranges and the settings schema (the search index and SETTING_META) and checks that a page can not offer a value
// the host refuses: every range of the host is a rule here, every number the schema offers is bounded by it, and every numeric field renders the rule.
const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')
const host = readFileSync(join(WEB, '../../../../src/config.cpp'), 'utf8')
const body = host.slice(host.search(/void\s+apply_config\(std::unordered_map/), host.indexOf('parse(int argc, char *argv[])'))
const ranges = {}
for (const m of body.matchAll(/(int_between_f|double_between_f)\(\s*vars,\s*"([a-z0-9_]+)"s?,\s*[^,]+,\s*\{\s*([^}]+)\}/g)) ranges[m[2]] = m[3].replace(/\s+/g, ' ').trim().split(/\s*,\s*/)

// the ranges the host writes with a name or an expression: what they come to
const NAMED = {
  port: ['1019', '65514'], // 1024 + PORT_HTTPS (-5), 65535 - RTSP_SETUP_PORT (21); saveConfig takes 1029..65514 (the rule here)
  sleep_mode: ['0', '2'], // SLEEP_MODE_SUSPEND..SLEEP_MODE_AWAY
  ping_timeout: ['-1', '2147483647'],
  key_repeat_frequency: ['0', 'max'],
  video_pacing_threshold: ['0', '16777216'], // 0..16 * 1024 * 1024 (the host writes the product)
}

test('every range of the host (apply_config) is a rule of the page with the same numbers', () => {
  assert.ok(Object.keys(ranges).length > 25)
  for (const [key, [lo, hi]] of Object.entries(ranges)) {
    const rule = CONSTRAINTS[key]
    assert.ok(rule, `${key}: the host takes ${lo}..${hi} and the page has no rule`)
    if (NAMED[key]) continue
    assert.deepEqual([rule.min, rule.max], [Number(lo), Number(hi)], `${key}`)
  }
  for (const key of Object.keys(NAMED)) assert.ok(CONSTRAINTS[key], key)
})

test('the base port is the range saveConfig takes, and what apply_config would take is wider', () => {
  const save = readFileSync(join(WEB, '../../../../src/confighttp/configuration.cpp'), 'utf8')
  assert.match(save, /port < 1029 \|\| port > 65514/)
  assert.deepEqual([CONSTRAINTS.port.min, CONSTRAINTS.port.max], [1029, 65514])
  assert.ok(Number(NAMED.port[0]) <= CONSTRAINTS.port.min && Number(NAMED.port[1]) >= CONSTRAINTS.port.max)
  for (const key of ['sunshine_name', 'adapter_name']) assert.match(save, new RegExp(`${key}.*too long \\(max 256\\)`))
  assert.equal(CONSTRAINTS.sunshine_name.maxLength, 256)
})

test('a slider of the schema stays inside what the host takes', () => {
  const problems = []
  for (const [key, meta] of Object.entries(SETTING_META)) {
    const rule = CONSTRAINTS[key]
    if (meta.control !== 'slider' || !rule || rule.min === undefined || meta.min === undefined) continue
    // a slider with a value for "off" or "automatic" below its range keeps that value outside the range on purpose (the host reads it as its default)
    // (an off position that clears the value (offWrite) is not a value: the first real one is a step above it)
    const lo = meta.offWrite !== undefined ? meta.min + (meta.step || 1) : meta.off !== undefined ? Math.min(meta.min, meta.off) : meta.min
    if (!(lo >= rule.min && meta.max <= rule.max)) problems.push(`${key}: the slider goes ${meta.min}..${meta.max}, the host takes ${rule.min}..${rule.max}`)
  }
  assert.deepEqual(problems, [])
})

const files = (dir) => readdirSync(dir).flatMap((n) => (statSync(join(dir, n)).isDirectory() ? files(join(dir, n)) : n.endsWith('.vue') ? [join(dir, n)] : []))
const templateOf = (s) => s.slice(s.indexOf('<template>'))

test('every number field of the settings is a key with a rule, and its component shows the rule', () => {
  const numeric = new Set()
  for (const f of files(join(WEB, 'configs'))) {
    for (const tag of tokenize(templateOf(readFileSync(f, 'utf8')))) {
      if (tag.closing) continue
      const id = tag.attrs.k || tag.attrs['data-cfg']
      if (id && tag.attrs.type === 'number') numeric.add(id)
    }
  }
  assert.ok(numeric.has('port') && numeric.has('usb_forwarding_port') && numeric.has('dsu_server_port'))
  for (const id of numeric) assert.ok(CONSTRAINTS[id], `${id} is a number field and the page has no rule for it`)
  for (const name of ['CfgField', 'CfgExpert']) assert.match(readFileSync(join(WEB, `components/cfg/${name}.vue`), 'utf8'), /validateSetting\(/, `${name} does not show the rule`)
})

test('the schema numbers that have no range at the host are whole numbers at least', () => {
  for (const key of ['qp', 'min_threads', 'max_bitrate', 'back_button_timeout', 'key_repeat_delay']) {
    assert.equal(validateSetting(key, 10), null)
    assert.equal(validateSetting(key, '1.5').code, 'integer')
    assert.equal(validateSetting(key, 'abc').code, 'number')
  }
  assert.ok(SEARCH_INDEX.length > 100)
})

test('the unsaved list marks a change the host would refuse', () => {
  const src = readFileSync(join(WEB, 'composables/pendingSources.js'), 'utf8')
  assert.match(src, /invalid: invalidOf\(e\.key, e\.after\)/)
  assert.match(src, /validateSetting\(key, value\)/)
})
