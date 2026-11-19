import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import test from 'node:test'

import { DS5_SUB, REASONS, ds5Availability, reasonText } from 'foundation-ui/settings/index.js'

const read = (path) => readFile(new URL(path, import.meta.url), 'utf8')
const locale = async (name) => JSON.parse(await read(`../public/assets/locale/${name}.json`))

// the keys the host reports as unavailable (tests/unit/test_ds5_tuning.cpp) with the cause code it sends for each
const HOST_REASONS = [
  [10, 'transport'], [98, 'transport'], [100, 'transport'],
  [31, 'slope'], [97, 'preamp'],
  [102, 'control-channel'], [103, 'control-channel'], [104, 'control-channel'],
  [105, 'client'],
]

test('every key the host cannot apply says its real cause in the words of the person, never "not open yet"', () => {
  for (const loc of ['zh', 'en']) {
    for (const [id, code] of HOST_REASONS) {
      const availability = ds5Availability({ id, key: `ds5.k${id}`, status: 'unavailable', implemented: false, reason_code: code }, { locale: loc })
      assert.equal(availability.state, 'unavailable', `${id}`)
      assert.equal(availability.code, code, `${id}`)
      assert.ok(availability.reason.length > 4, `${id}`)
      assert.ok(!/暂未开放|暫未開放|尚未提供|not (open|available) yet/i.test(availability.reason), `${id}: ${availability.reason}`)
      assert.equal(availability.reason, reasonText(code, loc))
    }
  }
  assert.ok(Object.keys(REASONS).includes('control-channel'))
})

test('the strings of the DualSense page no longer say 暂未开放 or 尚未提供', async () => {
  for (const name of ['zh', 'zh_TW', 'en', 'en_GB', 'en_US']) {
    const ds5 = (await locale(name)).ds5
    const text = JSON.stringify(ds5)
    assert.ok(!/暂未开放|暫未開放|尚未提供|尚未接入|not available yet|not connected in this build yet/i.test(text), `${name}: ${text.match(/.{20}(暂未开放|暫未開放|尚未提供|尚未接入|not available yet|not connected in this build yet).{10}/i)?.[0]}`)
  }
})

test('a disabled row shows the reason of its key, from the host\'s code', async () => {
  const field = await read('../components/dualsense/ControlField.vue')
  assert.match(field, /ds5Availability\(props\.descriptor/)
  assert.match(field, /:text="disabledReason \|\| /)
  assert.ok(!/ds5\.pending\.reason/.test(field), 'the row no longer uses the one sentence for every key')
})

test('the page of the unavailable keys is not behind the advanced warning and is always there', async () => {
  const subs = await read('../components/dualsense/DualSenseSubs.vue')
  assert.match(subs, /sub !== DS5_SUB\.pending/)
  const opened = subs.indexOf('<template v-if="advancedOpen">')
  const pending = subs.indexOf('<CfgSub :id="DS5_SUB.pending"')
  assert.ok(pending > 0 && opened > 0 && pending < opened, 'the pending page is outside the block that waits for the warning')
  assert.equal(DS5_SUB.isGroup(DS5_SUB.pending), true, 'foundation-ui still counts it as a group: the page does the exception')
})

test('the test page sends real tests (rumble, light bar, triggers) and says what the host did', async () => {
  const subs = await read('../components/dualsense/DualSenseSubs.vue')
  assert.match(subs, /\/api\/dualsense\/rumble-test/)
  assert.match(subs, /\/api\/dualsense\/pad-test/)
  assert.ok(!/preview_only/.test(subs))
  for (const name of ['zh', 'zh_TW', 'en']) {
    const test_panel = (await locale(name)).ds5.test_panel
    for (const result of ['sent', 'no_pad', 'unsupported', 'busy', 'failed']) assert.ok(test_panel.result[result], `${name} ${result}`)
    for (const action of ['identify', 'trigger_left', 'trigger_right', 'stop']) assert.ok(test_panel[action], `${name} ${action}`)
    for (const action of ['identify', 'trigger_left', 'trigger_right', 'stop']) assert.ok(test_panel.sent[action], `${name} sent ${action}`)
    assert.ok(!/暂未开放|演示|仅预览|preview only/i.test(JSON.stringify(test_panel)), name)
  }
})

test('a key only a client can finish says which clients do, from which version', async () => {
  const field = await read('../components/dualsense/ControlField.vue')
  assert.match(field, /descriptor\.client_support/)
  assert.match(field, /:text="disabledReason \|\| supportText \|\| undefined"/)
  for (const name of ['zh', 'zh_TW', 'en']) {
    const support = (await locale(name)).ds5.client_support
    assert.match(support.needs, /\{clients\}/, name)
    assert.match(support.since, /\{version\}/, name)
  }
  assert.match((await locale('zh')).ds5.client_support.needs, /^需要客户端支持/)
})
