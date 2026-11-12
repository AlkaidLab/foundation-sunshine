import assert from 'node:assert/strict'
import { readdir, readFile } from 'node:fs/promises'
import test from 'node:test'

import {
  ADVANCED_WARNING_HIDDEN_KEY,
  ADVANCED_WARNING_SECONDS,
  isAdvancedWarningHidden,
  remainingSeconds,
  setAdvancedWarningHidden,
} from '../utils/advancedWarning.js'

const localeDirectory = new URL('../public/assets/locale/', import.meta.url)

const memoryStorage = () => {
  const map = new Map()
  return {
    getItem: (key) => (map.has(key) ? map.get(key) : null),
    setItem: (key, value) => map.set(key, String(value)),
    removeItem: (key) => map.delete(key),
  }
}

const refusingStorage = () => ({
  getItem() { throw new Error('denied') },
  setItem() { throw new Error('denied') },
  removeItem() { throw new Error('denied') },
})

test('the warning shows until the browser stores the opt-out', () => {
  const storage = memoryStorage()
  assert.equal(isAdvancedWarningHidden(storage), false)
  setAdvancedWarningHidden(storage, true)
  assert.equal(storage.getItem(ADVANCED_WARNING_HIDDEN_KEY), '1')
  assert.equal(isAdvancedWarningHidden(storage), true)
  setAdvancedWarningHidden(storage, false)
  assert.equal(isAdvancedWarningHidden(storage), false)
})

test('only the stored value 1 counts as an opt-out', () => {
  const storage = memoryStorage()
  storage.setItem(ADVANCED_WARNING_HIDDEN_KEY, 'true')
  assert.equal(isAdvancedWarningHidden(storage), false)
  assert.equal(isAdvancedWarningHidden(null), false)
  assert.equal(isAdvancedWarningHidden(undefined), false)
})

test('blocked storage keeps the warning on and does not throw', () => {
  assert.equal(isAdvancedWarningHidden(refusingStorage()), false)
  assert.doesNotThrow(() => setAdvancedWarningHidden(refusingStorage(), true))
  assert.doesNotThrow(() => setAdvancedWarningHidden(null, true))
})

test('the confirm countdown runs five whole seconds and then unlocks', () => {
  assert.equal(ADVANCED_WARNING_SECONDS, 5)
  assert.equal(remainingSeconds(0), 5)
  assert.equal(remainingSeconds(1), 5)
  assert.equal(remainingSeconds(999), 5)
  assert.equal(remainingSeconds(1000), 4)
  assert.equal(remainingSeconds(4001), 1)
  assert.equal(remainingSeconds(4999), 1)
  assert.equal(remainingSeconds(5000), 0)
  assert.equal(remainingSeconds(60000), 0)
  assert.equal(remainingSeconds(-250), 5)
})

test('every locale carries the complete warning text', async () => {
  const keys = ['title', 'intro', 'risk_input', 'risk_disconnect', 'risk_audio', 'risk_later', 'outro', 'dont_show', 'confirm', 'confirm_wait']
  const files = (await readdir(localeDirectory)).filter((name) => name.endsWith('.json'))
  assert.ok(files.length >= 20)
  const english = JSON.parse(await readFile(new URL('en.json', localeDirectory), 'utf8')).ds5.advanced_warning

  for (const file of files) {
    const warning = JSON.parse(await readFile(new URL(file, localeDirectory), 'utf8')).ds5?.advanced_warning
    assert.ok(warning, `${file} is missing ds5.advanced_warning`)
    for (const key of keys) {
      assert.equal(typeof warning[key], 'string', `${file} is missing ds5.advanced_warning.${key}`)
      assert.ok(warning[key].trim().length > 0, `${file} has an empty ds5.advanced_warning.${key}`)
    }
    assert.match(warning.confirm_wait, /\{seconds\}/, `${file} confirm_wait has no {seconds}`)
    assert.deepEqual(Object.keys(warning).sort(), keys.slice().sort(), `${file} has unexpected warning keys`)
    if (!file.startsWith('en')) {
      assert.notEqual(warning.intro, english.intro, `${file} still has the English warning text`)
    }
  }
})

test('the Chinese checkbox label is the specified one', async () => {
  const zh = JSON.parse(await readFile(new URL('zh.json', localeDirectory), 'utf8'))
  assert.equal(zh.ds5.advanced_warning.dont_show, '下次不再显示此警告')
})

test('the page asks before opening advanced options and never restores them past the warning', async () => {
  const view = await readFile(new URL('../components/dualsense/DualSenseGamepad.vue', import.meta.url), 'utf8')
  const subs = await readFile(new URL('../components/dualsense/DualSenseSubs.vue', import.meta.url), 'utf8')
  const toggle = view.match(/const toggleAdvanced = \(\) => \{[\s\S]*?\n\}\n/)?.[0] ?? ''
  assert.ok(toggle, 'toggleAdvanced is missing')
  // Opening is only reachable through the opt-out check; the only unconditional write closes the section.
  assert.match(toggle, /warningHidden\(\)/)
  assert.match(toggle, /advancedWarningOpen\.value = true/)
  const opens = toggle.match(/advancedOpen\.value = true/g) ?? []
  assert.equal(opens.length, 1)
  assert.ok(toggle.indexOf('warningHidden()') < toggle.indexOf('advancedOpen.value = true'))
  assert.match(view, /<AdvancedWarningDialog[\s\S]*?@confirm="confirmAdvanced"/)
  assert.match(view, /localStorage\.getItem\('ds5\.advanced\.open'\) === '1' && store\.warningHidden\(\)/)
  const writes = view.match(/advancedOpen\.value = true/g) ?? []
  assert.equal(writes.length, 2, 'advanced options may only open from the toggle or the confirmed warning (the restore is gated by the opt-out check above)')
  // a link, a search hit or a reload that lands on an advanced group asks first; cancelling returns to the gamepad page
  assert.match(subs, /if \(store\.warningHidden\(\)\) advancedOpen\.value = true\n  else warningOpen\.value = true/)
  assert.match(subs, /const cancelWarning = \(\) => \{ warningOpen\.value = false; nav\.sub\.value = '' \}/)
  assert.match(subs, /<template v-if="advancedOpen">\s*<CfgSub v-for="group in advancedGroups"/)
})

test('the dialog disables confirm until the countdown ends', async () => {
  const dialog = await readFile(new URL('../components/dualsense/AdvancedWarningDialog.vue', import.meta.url), 'utf8')
  assert.match(dialog, /:disabled="!ready"/)
  assert.match(dialog, /const ready = computed\(\(\) => seconds\.value === 0\)/)
  assert.match(dialog, /if \(ready\.value\) emit\('confirm', dontShow\.value\)/)
  assert.match(dialog, /<md-checkbox/)
})
