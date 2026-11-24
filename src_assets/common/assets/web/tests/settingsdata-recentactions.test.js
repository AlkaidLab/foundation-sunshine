// SPDX-License-Identifier: GPL-3.0-only
// The checks of recentactions.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { APPLIES, appliesOf, appliesText } = await import('foundation-ui/settings/applies.js')

test('every setting of the index has a class of when it applies, from the host audit, and a line for it in both languages', () => {
  for (const e of SEARCH_INDEX) for (const lang of ['zh', 'en']) assert.ok(Object.values(APPLIES).includes(appliesOf(e.id, 'windows')) && appliesText(e.id, 'windows', lang), `${e.id} ${lang}`)
  assert.equal(appliesOf('encoder'), APPLIES.RESTART); assert.equal(appliesOf('controller'), APPLIES.RESTART); assert.equal(appliesOf('stream_audio'), APPLIES.RESTART)
  assert.equal(appliesOf('max_bitrate'), APPLIES.NOW); assert.equal(appliesOf('fps'), APPLIES.CLIENT); assert.equal(appliesOf('hevc_mode'), APPLIES.CLIENT)
  assert.equal(appliesOf('nvenc_manage_driver_settings'), APPLIES.NEXT); assert.equal(appliesOf('nvenc_vdd_force_vsync'), APPLIES.NEXT)
  assert.equal(appliesOf('gamepad', 'windows'), APPLIES.LIVE); assert.equal(appliesOf('gamepad', 'linux'), APPLIES.RESTART); assert.equal(appliesOf('gamepad', 'macos'), APPLIES.RESTART); assert.equal(appliesOf('gamepad', ''), APPLIES.RESTART)
  for (const k of ['external_endpoints', 'hdr_enhanced', 'notify_pre_releases', 'vdd_keep_enabled', 'vdd_headless_create']) assert.equal(appliesOf(k), APPLIES.UNCLEAR, k)
  assert.equal(appliesText('gamepad', 'windows', 'zh'), '新建的手柄生效'); assert.equal(appliesText('max_bitrate', '', 'zh'), '立即生效,正在串流的会话也会变'); assert.equal(appliesText('fps', '', 'zh'), '由客户端决定 · 重启后生效'); assert.equal(appliesText('encoder', '', 'en'), 'After a restart')
})
