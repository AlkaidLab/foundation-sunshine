import { test } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import './helpers/foundationUi.mjs'

const { bitrateChips, bitrateFromMbps, bitrateKbps, bitrateSliderMbps, bitrateText } = await import('foundation-ui/settings/bitrate.js')

// 音频/视频 › 最大比特率 (max_bitrate, Kbps; 0 = 自动). Report 2026-11-08 「默认莫名其妙变成了65」「自动的时候这里显示有问题」: the name 自动 sat under the slider's icon, the slider and its bubble read "0 Mbps" at automatic,
// and a custom chip wrote (Kbps / 1000) * 1000 back, which is not the stored number for 5893 of the values up to 300 Mbps.
const read = (f) => fs.readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')
const src = read('configs/tabs/audiovideo/BitrateControl.vue')

test('the control takes its rules from foundation-ui settings/bitrate.js, the ones of the panel 串流 page', () => {
  assert.match(src, /from 'foundation-ui\/settings'/)
  assert.doesNotMatch(src, /exact\.value \/ 1000|Number\(value\) \* 1000|PRESETS = \[/, 'no private float arithmetic or preset list left')
  assert.match(src, /const kbps = computed\(\(\) => bitrateKbps\(props\.config\.max_bitrate\)\)/)
  assert.match(src, /set: \(value\) => \{ props\.config\.max_bitrate = bitrateFromProgress\(value\) \}/)
  assert.match(src, /set: \(value\) => \{ props\.config\.max_bitrate = bitrateKbps\(value\) \}/)
})

test('fresh config, 0, a preset and a custom value: what shows and what a chip or the slider writes', () => {
  // the host's /api/config has no max_bitrate on a fresh install and answers strings otherwise
  for (const raw of [undefined, '0', 0, '']) { assert.equal(bitrateKbps(raw), 0); assert.equal(bitrateText(raw, '自动'), '自动'); assert.equal(bitrateSliderMbps(raw), 0) }
  assert.deepEqual(bitrateChips(undefined, '自动').map((c) => c.label), ['自动', '20 Mbps', '40 Mbps', '80 Mbps', '120 Mbps', '200 Mbps'])
  assert.equal(bitrateKbps('80000'), 80000); assert.equal(bitrateSliderMbps('80000'), 80)
  const custom = bitrateChips('65432', '自动')
  assert.ok(custom.some((c) => c.value === 65432 && c.label === '65.4 Mbps'), 'the custom value is a chip and is chosen by equality')
  assert.equal(bitrateKbps(custom.find((c) => c.value === 65432).value), 65432, 'choosing the custom chip writes exactly the stored number')
  assert.equal(bitrateFromMbps(65), 65000)
})

test('the slider says 自动 at its lowest position and the value text agrees (shared with the panel)', () => {
  assert.match(src, /<MSlider v-model="progress" :min="0" :max="BITRATE_SLIDER_MAX_PROGRESS" :step="1" unit="" size="m" icon-start="signal_cellular_alt_2_bar" icon-end="signal_cellular_alt" :marks="chipMarks"/)
  assert.match(src, /\{\{ bitrateText\(kbps, \$t\('_common\.auto'\)\) \}\}/)
})
