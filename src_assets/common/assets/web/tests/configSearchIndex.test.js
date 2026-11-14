import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { buildConfigIndex, localeKeyOf, renderIndexFile, tokenize } from '../../../../../scripts/gen-config-index.mjs'
// the index and the settings model live in the foundation-ui package (the sibling checkout), shared with the control panel
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX } = await import('foundation-ui/settings/searchIndex.js')
const { CONFIG_TAB_IDS, parseSettingsHash } = await import('foundation-ui/settings/model.js')

const here = dirname(fileURLToPath(import.meta.url))
const foundationUi = process.env.FOUNDATION_UI_DIR || join(here, '../../../../../../ds5-ui-m3e')

const locale = JSON.parse(readFileSync(new URL('../public/assets/locale/en.json', import.meta.url), 'utf8'))
const lookup = (key) => key.split('.').reduce((node, part) => (node && typeof node === 'object' ? node[part] : undefined), locale) !== undefined
// a key can exist only as its platform variants (`$tp`)
const hasKey = (key) => lookup(key) || lookup(`${key}_windows`) || lookup(`${key}_unix`)

test('the tokenizer reads attributes with quotes and a greater-than inside', () => {
  const [tag] = tokenize('<CfgField k="port" :disabled="a > b" help=\'none\' />')
  assert.equal(tag.name, 'CfgField')
  assert.equal(tag.attrs.k, 'port')
  assert.equal(tag.attrs[':disabled'], 'a > b')
  assert.equal(tag.attrs.help, 'none')
  assert.ok(tag.selfClosing)
})

test('only a plain locale expression is a title key', () => {
  assert.equal(localeKeyOf("$t('config.port')"), 'config.port')
  assert.equal(localeKeyOf("$tp('config.output_name_desc')"), 'config.output_name_desc')
  assert.equal(localeKeyOf("`${a}`"), '')
  assert.equal(localeKeyOf("$t('a') + $t('b')"), '')
})

test('the committed index is what the templates generate', () => {
  assert.equal(renderIndexFile(buildConfigIndex()), readFileSync(join(foundationUi, 'src/settings/searchIndex.js'), 'utf8'))
})

test('every entry opens a tab that exists and names locale keys that exist', () => {
  assert.ok(SEARCH_INDEX.length > 100)
  const tabs = new Set(CONFIG_TAB_IDS)
  const ids = new Set()
  for (const entry of SEARCH_INDEX) {
    assert.ok(!ids.has(entry.id), `duplicate ${entry.id}`)
    ids.add(entry.id)
    assert.equal(entry.route, 'config')
    assert.ok(tabs.has(entry.params.tab), entry.id)
    assert.equal(entry.params.focus, entry.id)
    for (const key of [entry.title, ...entry.path]) assert.ok(hasKey(key), `${entry.id}: missing locale key ${key}`)
  }
})

test('known settings land on their section', () => {
  const at = (id) => SEARCH_INDEX.find((entry) => entry.id === id)?.params
  assert.deepEqual(at('bind_address'), { tab: 'network', focus: 'bind_address' }, 'the advanced settings are part of the tab page')
  assert.deepEqual(at('nvenc_preset'), { tab: 'display', sub: 'encoders', focus: 'nvenc_preset' })
  assert.deepEqual(at('locale'), { tab: 'general', focus: 'locale' })
  assert.deepEqual(at('hevc_mode'), { tab: 'display', sub: 'encoders', focus: 'hevc_mode' })
  assert.deepEqual(at('stream_audio'), { tab: 'audio', sub: 'sound', focus: 'stream_audio' }, 'the audio settings are on 音频')
  assert.deepEqual(at('hdr_prep'), { tab: 'display', sub: 'hdr', focus: 'hdr_prep' }, 'the display settings are on 显示与视频')
  assert.ok(!SEARCH_INDEX.some((entry) => ['av', 'advanced', 'encoders'].includes(entry.params.tab)), 'the old tabs are gone')
  assert.deepEqual(parseSettingsHash('#fec_percentage', SEARCH_INDEX), { tab: 'network', focus: 'fec_percentage' })
  // an old address that names the advanced section still opens the tab page and jumps to the row
  assert.deepEqual(parseSettingsHash('#tab=network&sub=expert&focus=fec_percentage', SEARCH_INDEX), { tab: 'network', sub: 'expert', focus: 'fec_percentage' })
})
