import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test, { after, before } from 'node:test'
import { createRequire } from 'node:module'
import { startHarness } from './helpers/editorHarness.js'
import { findAll } from './helpers/fakeDom.js'

// The cover section of the editor (ImageSelector twice, CoverFinder, the Sparxie cover choice), mounted on the small DOM of helpers/fakeDom.js. The texts are the keys: the harness has no messages.
const read = (path) => readFileSync(new URL(`../${path}`, import.meta.url), 'utf8')
let h
let Selector
before(async () => {
  h = await startHarness()
  Selector = (await h.load('/components/ImageSelector.vue')).default
})
after(async () => {
  await h?.close()
  setTimeout(() => process.exit(process.exitCode ?? 0), 800).unref()
})

const mountSelector = async (props) => {
  const events = []
  const page = await h.mount(Selector, props, { updateImage: (v) => events.push(['update-image', v]), imageError: (v) => events.push(['image-error', v]) })
  return { page, events }
}

test('a portrait cover that is set: choose, find and clear are on the card, the path is in the field, nothing is emitted by opening it', async () => {
  const { page, events } = await mountSelector({ kind: 'poster', title: 'Portrait', imagePath: 'cover.png', appName: 'App' })
  for (const label of ['apps.choose_image', 'apps.find_cover', 'apps.image_clear']) assert.equal(page.buttons(label).length, 1, label)
  assert.equal(events.length, 0)
  await page.press(page.button('apps.image_clear'))
  assert.deepEqual(events, [['update-image', '']])
  page.unmount()
})

test('the wallpaper switch sets the path to desktop; with it on there is no file choice and no clear', async () => {
  const off = await mountSelector({ kind: 'poster', imagePath: '', appName: 'App' })
  assert.equal(off.page.buttons('apps.image_clear').length, 0, 'nothing to clear on an empty cover')
  assert.ok(off.page.text().includes('apps.choose_image'), 'the empty frame invites a file')
  await off.page.press(off.page.switches('apps.use_desktop_image')[0])
  assert.deepEqual(off.events, [['update-image', 'desktop']])
  off.page.unmount()
  const on = await mountSelector({ kind: 'poster', imagePath: 'desktop', appName: 'App' })
  for (const label of ['apps.choose_image', 'apps.find_cover', 'apps.image_clear']) assert.equal(on.page.buttons(label).length, 0, label)
  assert.ok(on.page.text().includes('apps.use_desktop_image'))
  on.page.unmount()
})

test('the landscape cover has no wallpaper switch and no finder, and its frame is 16:9', async () => {
  const { page } = await mountSelector({ kind: 'hero', title: 'Landscape', imagePath: 'hero.png', appName: 'App' })
  assert.equal(page.switches('apps.use_desktop_image').length, 0)
  assert.equal(page.buttons('apps.find_cover').length, 0)
  assert.equal(page.buttons('apps.choose_image').length, 1)
  assert.ok(page.text().includes('16:9'))
  page.unmount()
})

test('a finder that finds nothing, fails or is not asked yet each has its own state; picking a cover shows the apply state and cannot be repeated', () => {
  const finder = read('components/CoverFinder.vue')
  assert.match(finder, /side="center"/, 'a centred M3 dialog, a bottom sheet on a phone')
  for (const state of ['v-if="loading"', 'v-else-if="failed"', 'v-else-if="filteredCovers.length"', 'v-else-if="searched"', 'v-if="applying"']) assert.ok(finder.includes(state), state)
  assert.match(finder, /if \(this\.applying\) return/)
  assert.match(finder, /this\.failed = true/)
})

test('an official Sparxie cover previews the poster and the hero the host made, plain or badged, and the badge switch changes both at once', async () => {
  const { COPY } = await h.load('/utils/sparxie/copy.js')
  const Cover = (await h.load('/components/sparxie/SparxieCover.vue')).default
  const { reactive } = createRequire(import.meta.url)('vue')
  const form = reactive({ sparxie: { cover: 'official', badge: true } })
  const page = await h.mount(Cover, { form, touch: true, artId: 'genshin-cn', artVersion: 'ab12', updated: '11-10' })
  const srcs = () => findAll(page.root, (n) => n.tagName === 'img').map((n) => n.attrs.src)
  assert.deepEqual(srcs(), ['/api/sparxie/art/card/genshin-cn?v=ab12&badge=1', '/api/sparxie/art/hero/genshin-cn?v=ab12&badge=1'])
  assert.ok(page.text().includes('16:9') && page.text().includes('2:3'))
  assert.equal((page.text().match(new RegExp(COPY.zh.coverUpdated('11-10'), 'g')) || []).length, 2, 'the date is the supporting line of each card')
  await page.press(page.switches(COPY.zh.posterBadge)[0])
  assert.equal(form.sparxie.badge, false)
  assert.deepEqual(srcs(), ['/api/sparxie/art/card/genshin-cn?v=ab12', '/api/sparxie/art/hero/genshin-cn?v=ab12'])
  // a cover of the person's own is not previewed here (the picker below it shows the image); without the touch input the posters are plain whatever the setting
  await page.press(page.switches(COPY.zh.coverFollow)[0])
  assert.equal(srcs().length, 0)
  assert.equal(form.sparxie.cover, 'custom')
  page.unmount()
  const plain = await h.mount(Cover, { form: reactive({ sparxie: { cover: 'official', badge: true } }), touch: false, artId: 'genshin-cn', artVersion: 'ab12' })
  assert.deepEqual(findAll(plain.root, (n) => n.tagName === 'img').map((n) => n.attrs.src), ['/api/sparxie/art/card/genshin-cn?v=ab12', '/api/sparxie/art/hero/genshin-cn?v=ab12'])
  plain.unmount()
})

test('a cover of the person\'s own shows the Sparxie mark on its preview only when the badge is on, and the badge asks the host for badged art', async () => {
  const withBadge = await mountSelector({ kind: 'poster', imagePath: 'own.png', appName: 'App', badge: true })
  assert.equal(findAll(withBadge.page.root, (n) => n.tagName === 'span' && /cover-badge/.test(String(n.className || n.attrs.class || ''))).length, 1)
  withBadge.page.unmount()
  const without = await mountSelector({ kind: 'hero', imagePath: 'own.png', appName: 'App' })
  assert.equal(findAll(without.page.root, (n) => n.tagName === 'span' && /cover-badge/.test(String(n.className || n.attrs.class || ''))).length, 0)
  without.page.unmount()
  const { artUrl } = await h.load('/utils/sparxie/model.js')
  assert.equal(artUrl('hero', { id: 'x' }, 'v', true), '/api/sparxie/art/hero/x?v=v&badge=1')
  assert.equal(artUrl('hero', { id: 'x' }, '', true), '/api/sparxie/art/hero/x?badge=1')
  assert.equal(artUrl('card', { id: 'x' }, 'v'), '/api/sparxie/art/card/x?v=v')
})
