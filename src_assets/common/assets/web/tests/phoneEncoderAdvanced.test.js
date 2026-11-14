import assert from 'node:assert/strict'
import { createRequire } from 'node:module'
import { readFileSync } from 'node:fs'
import test, { after, before } from 'node:test'
import { startHarness } from './helpers/editorHarness.js'
import { FakeNode, findAll, isDisabled } from './helpers/fakeDom.js'
import zh from '../public/assets/locale/zh.json' with { type: 'json' }
import { labelPx, segFits, segNeed } from '../composables/useSegFit.js'

// Reported 2026-11-12: the encoder and advanced settings could not be changed on a phone. On a phone (the window under 600 px: the shell is compact, 设置 is a hub, a page is two presses away) the rows of the encoder and
// the advanced pages are mounted on the small DOM at the widths of the phones (360, 390, 430 px), and every choice must be reachable and operable:
//   - a choice drawn as buttons shows every name whole and every button is a finger wide (the 7 presets of NVENC ran out of the window: P7 could not be pressed; 四分之一分辨率 was cut off)
//   - a choice that does not fit is the value row that opens the list, so every answer is still one press and a pick away
//   - every button of every group can be pressed and sets the setting; every switch toggles
// and the desktop (a 1280 px window) keeps its groups. The fake DOM has no layout: `layout()` gives every row the width a phone gives it.
const require = createRequire(import.meta.url)
const { reactive } = require('vue')

let h
let Nvenc
let Advanced
before(async () => {
  h = await startHarness()
  Nvenc = (await h.load('/configs/tabs/encoders/NvidiaNvencEncoder.vue')).default
  Advanced = (await h.load('/configs/tabs/display/DisplayAdvanced.vue')).default
})
after(async () => {
  await h?.close()
  setTimeout(() => process.exit(process.exitCode ?? 0), 800).unref()
})

// every row is as wide as the window less the 12 px margins of the page
const layout = (windowWidth) => {
  globalThis.innerWidth = windowWidth
  FakeNode.prototype.getBoundingClientRect = () => { const w = windowWidth - 24; return { x: 0, y: 0, width: w, height: 56, top: 0, left: 0, right: w, bottom: 56 } }
}
const ancestor = (node, test) => { for (let n = node; n; n = n.parentNode) if (n.nodeType === 1 && test(n)) return n; return null }
const keyOf = (node) => ancestor(node, (n) => n.attrs['data-cfg'])?.attrs['data-cfg']
const groups = (root) => findAll(root, (n) => n.attrs.role === 'radiogroup').map((group) => {
  const radios = findAll(group, (n) => n.attrs.role === 'radio')
  return { group, key: keyOf(group), radios, labels: radios.map((r) => r.attrs['aria-label']), steps: !!ancestor(group, (n) => /\bcfg-steps\b/.test(n.className || n.attrs.class || "")) }
})

for (const phone of [360, 390, 430]) {
  test(`${phone} px: every choice of the NVENC page is whole and a finger wide, or a row that opens its list`, async () => {
    layout(phone)
    const config = reactive({ encoder: 'nvenc', nvenc_preset: '1' })
    const page = await h.mount(Nvenc, { platform: 'windows', config }, {}, { messages: zh })
    const found = groups(page.root)
    assert.ok(found.length > 0, 'the page draws choices')
    for (const g of found) {
      // what the row has for the buttons: its width less 32 px of padding; the presets take the whole width but 12 px
      const avail = phone - 24 - (g.steps ? 12 : 32)
      const need = segNeed(g.labels, g.steps ? { pad: 4, gap: 2 } : undefined)
      assert.ok(need <= avail, `${g.key}: ${g.labels.join(' / ')} needs ${need} px of a ${avail} px row (cut off or out of the window)`)
    }
    // a choice that is not buttons is a row with a button that opens its list
    for (const key of ['nvenc_twopass', 'nvenc_rate_control', 'nvenc_split_encode', 'nvenc_preset']) {
      const row = findAll(page.root, (n) => n.attrs['data-cfg'] === key)[0]
      assert.ok(row, `${key} is on the page`)
      if (!found.some((g) => g.key === key)) assert.ok(findAll(row, (n) => n.tagName === 'button').length > 0, `${key} is not buttons and has no button that opens its list`)
    }
    page.unmount()
  })

  test(`${phone} px: every button of every choice sets its setting, every switch toggles`, async () => {
    layout(phone)
    const config = reactive({ encoder: 'nvenc' })
    const page = await h.mount(Nvenc, { platform: 'windows', config }, {}, { messages: zh })
    for (const g of groups(page.root)) {
      for (const radio of g.radios) {
        await page.press(radio)
        assert.equal(page.selected(radio), true, `${g.key}: ${radio.attrs['aria-label']} is not the chosen one after a press`)
      }
    }
    const sw = findAll(page.root, (n) => n.attrs.role === 'switch' && !isDisabled(n))
    assert.ok(sw.length >= 6, `the page has its switches (${sw.length})`)
    for (const s of sw) {
      if (isDisabled(s)) continue // the row follows another switch of the page (driver settings off)
      const before = s.attrs['aria-checked']
      await page.press(s)
      assert.notEqual(s.attrs['aria-checked'], before, `${keyOf(s)}: a press did not toggle the switch`)
    }
    page.unmount()
  })
}

test('the presets of NVENC are seven buttons on a phone of 390 px, P7 included, and the choice that cannot fit is a row', async () => {
  layout(390)
  const config = reactive({ encoder: 'nvenc' })
  const page = await h.mount(Nvenc, { platform: 'windows', config }, {}, { messages: zh })
  const preset = groups(page.root).find((g) => g.key === 'nvenc_preset')
  assert.ok(preset, 'the presets are buttons')
  assert.deepEqual(preset.labels, ['P1', 'P2', 'P3', 'P4', 'P5', 'P6', 'P7'])
  await page.press(preset.radios[6])
  assert.equal(config.nvenc_preset, '7')
  assert.equal(groups(page.root).some((g) => g.key === 'nvenc_twopass'), false, '四分之一分辨率 does not fit three buttons of a 390 px phone: the row opens its list')
  page.unmount()
})

test('the desktop keeps its groups: at 1280 px the same choices are buttons', async () => {
  layout(1280)
  const config = reactive({ encoder: 'nvenc' })
  const page = await h.mount(Nvenc, { platform: 'windows', config }, {}, { messages: zh })
  const keys = groups(page.root).map((g) => g.key)
  for (const key of ['nvenc_preset', 'nvenc_twopass', 'nvenc_rate_control']) assert.ok(keys.includes(key), `${key} is buttons on the desktop`)
  page.unmount()
})

test('the advanced page of 显示 on a phone: its choices fit and work, its switches toggle', async () => {
  layout(390)
  const config = reactive({})
  const page = await h.mount(Advanced, { platform: 'windows', config }, {}, { messages: zh })
  const found = groups(page.root)
  for (const g of found) {
    assert.ok(segNeed(g.labels) <= 390 - 24 - 32, `${g.key}: ${g.labels.join(' / ')} does not fit`)
    for (const radio of g.radios) { await page.press(radio); assert.equal(page.selected(radio), true, `${g.key}: ${radio.attrs['aria-label']}`) }
  }
  const sw = findAll(page.root, (n) => n.attrs.role === 'switch' && !isDisabled(n))
  for (const s of sw) { const before = s.attrs['aria-checked']; await page.press(s); assert.notEqual(s.attrs['aria-checked'], before, `${keyOf(s)}`) }
  assert.ok(found.length + sw.length > 0, 'the page has controls')
  page.unmount()
})

test('the rule itself: what a group of buttons needs', () => {
  assert.equal(labelPx('P1'), 16)
  assert.equal(labelPx('四分之一分辨率'), 98)
  assert.equal(segNeed(['P1', 'P2', 'P3', 'P4', 'P5', 'P6', 'P7'], { pad: 4, gap: 2 }), 7 * 44 + 12)
  assert.equal(segFits(['禁用', '四分之一分辨率', '全分辨率'], 334), false)
  assert.equal(segFits(['禁用', '四分之一分辨率', '全分辨率'], 600), true)
  assert.equal(segFits(['禁用', '四分之一分辨率', '全分辨率'], 0), true, 'width unknown: the group stays')
})

test('the style: on a phone the presets take the whole row with buttons that may be narrower than the 48 px of the touch layer', () => {
  const steps = readFileSync(new URL('../components/cfg/CfgSteps.vue', import.meta.url), 'utf8')
  assert.match(steps, /@media \(max-width: 599\.98px\)[^]*?\.cfg-steps \.m-seg \.m-seg__btn \{[^}]*min-width: 0/, 'a preset button may be narrower than 48 px')
  assert.match(steps, /\.m-item__control:has\(> \.cfg-steps\) \{ padding-left: 0; \}/, 'the presets are not indented by the icon on a phone')
})
