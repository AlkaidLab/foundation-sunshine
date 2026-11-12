import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { SEARCH_INDEX } from 'foundation-ui/settings/index.js'
import { tokenize } from '../../../../../scripts/gen-config-index.mjs'
import { HOT_KEYS } from '../composables/restartKeys.js'
import { NOT_A_KEY, OWN_SAVE, keysOf } from '../composables/settingsBinding.js'

// Every setting of the schema (the search index) is a control that writes a configuration key, so the unsaved store (which compares the model with what the host
// has) sees it and the one save writes it. A control that only wrote a local ref, or a key the host never reads, would change nothing that is saved, silently.
const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')
const host = readFileSync(join(WEB, '../../../../src/config.cpp'), 'utf8')
const body = host.slice(host.search(/void\s+apply_config\(std::unordered_map/), host.indexOf('parse(int argc, char *argv[])'))
const read = new Set([...body.matchAll(/"([a-z0-9_]+)"/g)].map((m) => m[1]))
for (const m of host.matchAll(/_f\(\s*vars,\s*"([a-z0-9_]+)"/g)) read.add(m[1])

// controls that reach the configuration through something other than `config.key =` in their own template: id -> [file, what it must contain]
const FILES_ROWS = ['file_apps', 'credentials_file', 'file_state', 'log_path', 'pkey', 'cert']
const INDIRECT = new Map([
  ['fps', ['configs/tabs/display/DisplayModes.vue', /props\.fps\.splice/]], // the list itself; useConfig serialises it into config.fps (checked below)
  ['resolutions', ['configs/tabs/display/DisplayModes.vue', /props\.resolutions\.splice/]],
  ['global_prep_cmd', ['configs/tabs/General.vue', /globalPrepCmd\.value\.(push|splice)/]],
  ['display_device_prep', ['configs/tabs/display/DisplayOutput.vue', /props\.config\.display_device_prep = value/]], // MPrepSeg, through a computed that writes the key
  // the video format and "allow HDR" are the two codec modes seen as one choice and as one flag (composables/useCodecFormat.js)
  ...['codec_strategy', 'codec_enable_hdr', 'hevc_mode', 'av1_mode'].map((id) => [id, ['composables/useCodecFormat.js', /Object\.assign\(config, modesFor\(/]]),
  // the two keys the tray owns are written through the virtual-display store (the tray action), as before
  ['vdd_headless_create', ['configs/tabs/display/DisplayOutput.vue', /store\.set\(\{ vdd_headless_create: value \}\)/]],
  ['vdd_keep_enabled', ['configs/tabs/display/DisplayVirtual.vue', /store\.set\(\{ vdd_keep_enabled: value \}\)/]],
  ['origin_web_ui_allowed', ['configs/tabs/Network.vue', /config\.value\.origin_web_ui_allowed = value/]], // MWebAccess: v-model:scope, staged like every setting
  ['bind_address', ['configs/tabs/Network.vue', /config\.value\.bind_address = value/]], // MWebAccess: v-model:bind (its advanced row)
  ...FILES_ROWS.map((id) => [id, ['configs/tabs/Files.vue', /props\.config\[editing\.value\]\s*=/]]),
])

const files = (dir) => readdirSync(dir).flatMap((n) => (statSync(join(dir, n)).isDirectory() ? files(join(dir, n)) : n.endsWith('.vue') ? [join(dir, n)] : []))
const sources = [...files(join(WEB, 'configs')), ...files(join(WEB, 'components/cfg'))].map((f) => ({ f, text: readFileSync(f, 'utf8') }))
const templateOf = (s) => s.slice(s.indexOf('<template>'))

// the tag that renders an id: a Cfg row (k), a custom control (data-cfg) or a component with an id
function renderedBy(id) {
  for (const { f, text } of sources) {
    for (const tag of tokenize(templateOf(text))) {
      if (tag.closing) continue
      if (/^Cfg(Switch|Choice|Field|Expert|Slider|Steps|Cards|Path|Device)$/.test(tag.name) && tag.attrs.k === id) return { kind: 'cfg', tag: tag.name, file: f }
      if (tag.attrs['data-cfg'] === id || (tag.name === 'AutomaticNumberSetting' && tag.attrs.id === id)) return { kind: 'custom', tag: tag.name, file: f, text }
    }
  }
  return null
}

test('a Cfg row component writes the key it is given into the configuration', () => {
  for (const name of ['CfgSwitch', 'CfgChoice', 'CfgField', 'CfgSlider', 'CfgSteps', 'CfgCards', 'CfgPath', 'CfgExpert', 'CfgDevice']) {
    const text = readFileSync(join(WEB, `components/cfg/${name}.vue`), 'utf8')
    assert.match(text, /(settings\(\)|props\.config)\[props\.k\]\s*=/, `${name} does not write config[k]`)
  }
})

test('every setting of the schema is rendered by a control that writes a key the host reads', () => {
  assert.ok(SEARCH_INDEX.length > 100)
  const problems = []
  for (const entry of SEARCH_INDEX) {
    const id = entry.id
    if (INDIRECT.has(id)) {
      const [file, pattern] = INDIRECT.get(id)
      if (!pattern.test(readFileSync(join(WEB, file), 'utf8'))) problems.push(`${id}: ${file} no longer has what writes it`)
      continue
    }
    const by = renderedBy(id)
    if (!by) { if (!NOT_A_KEY.has(id) && !/^(vdd_|dynamic_resolution|manual_|resolution_change|refresh_rate_change|output_name|variable_refresh)/.test(id)) problems.push(`${id}: no control renders it`); continue }
    const keys = keysOf(id)
    if (!OWN_SAVE.has(id) && keys.length) {
      for (const key of keys) {
        if (!read.has(key) && !HOT_KEYS.includes(key)) problems.push(`${id}: writes ${key}, which the host does not read`)
        if (by.kind === 'custom') {
          // an assignment (config.key = …, config['key'] = …) or a v-model on the key
          const binds = new RegExp(`(config|props\\.config)(\\.value)?(\\.${key}|\\[['"]${key}['"]\\])(\\s*=[^=]|\\s*\\+=|['"]\\s)`)
          if (!binds.test(by.text)) problems.push(`${id}: the control in ${by.file.split('/').slice(-2).join('/')} never assigns config.${key}`)
        }
      }
    }
  }
  assert.deepEqual(problems, [])
})

test('the controls that are not one key are the ones the schema says (codec strategy and its HDR switch write the two codec modes)', () => {
  const codec = readFileSync(join(WEB, '../../../../../ds5-ui-m3e/src/settings/codecFormat.js'), 'utf8')
  assert.match(readFileSync(join(WEB, 'composables/useCodecFormat.js'), 'utf8'), /Object\.assign\(config, modesFor\(/)
  for (const key of ['hevc_mode', 'av1_mode']) assert.match(codec, new RegExp(`${key}:`))
  assert.ok(read.has('hevc_mode') && read.has('av1_mode'))
})

test('the lists of the page (resolutions, frame rates, global commands) are compared and saved as the keys the host reads', () => {
  const cfg = readFileSync(join(WEB, 'composables/useConfig.js'), 'utf8')
  const compared = cfg.slice(cfg.indexOf('const currentForCompare'), cfg.indexOf('const setKey'))
  for (const part of ['serializeResolutions', 'serializeFps', 'global_prep_cmd']) assert.ok(compared.includes(part), `${part} is not part of what is compared`)
  for (const key of ['resolutions', 'fps', 'global_prep_cmd']) assert.ok(read.has(key), `${key} is not read by the host`)
})

test('the display encoders place keeps its page when another encoder is asked for (a deep link or a search hit to the AMF settings)', () => {
  const cfg = readFileSync(join(WEB, 'views/Config.vue'), 'utf8')
  assert.match(cfg, /:focus="focus"/)
  assert.doesNotMatch(cfg, /tab === 'encoders'/)
})
