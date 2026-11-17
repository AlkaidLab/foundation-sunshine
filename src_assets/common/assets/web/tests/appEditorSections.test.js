import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { sortCards } from '../utils/appsPage.js'

const editor = readFileSync(new URL('../components/AppEditor.vue', import.meta.url), 'utf8')
const template = editor.slice(editor.indexOf('<template>'), editor.indexOf('<script setup>'))

// every field of an application the old editor had is still in the new one, in one of the four sections
const FIELDS = ['name', 'output', 'cmd', 'working-dir', 'exclude-global-prep-cmd', 'prep-cmd', 'menu-cmd', 'detached', 'image-path', 'hero', 'elevated', 'gamepad', 'mouse-mode', 'auto-detach', 'wait-all', 'exit-timeout']
const RTX = ['mode', 'contrast', 'saturation', 'middle-gray', 'peak-nits']
const DLSS = ['mode', 'intensity', 'style', 'ui-correction', 'motion-quality']

test('every field of an application is kept by the sectioned editor', () => {
  for (const key of FIELDS) assert.ok(new RegExp(`formData(\\.${key.replace('-', '\\-')}\\b|\\['${key}'\\])`).test(template) || template.includes(`:${key === 'image-path' ? 'image-path' : key}=`) || (key === 'prep-cmd' && template.includes("formData['prep-cmd']")), `${key} is in the editor`)
  for (const key of RTX) assert.ok(template.includes(`formData['rtx-hdr']${/^[a-z]+$/.test(key) ? '.' + key : `['${key}']`}`), `rtx-hdr ${key}`)
  for (const key of DLSS) assert.ok(template.includes(`formData.dlssnr${/^[a-z]+$/.test(key) ? '.' + key : `['${key}']`}`), `dlssnr ${key}`)
})

test('the editor is a page: hero, the groups in order, the save bar, and the advanced groups behind the gate', () => {
  for (const marker of ['<MPage', 'apps-hero', 'apps-savebar', 'id="apps-cover"', "t('apps.section_launch')", "t('apps.command_settings')"]) assert.ok(template.includes(marker), marker)
  assert.ok(!template.includes('<MSheet') && !template.includes('apps-editor__tabs'), 'no sheet and no tabs')
  assert.ok(template.includes('v-if="!showAdvanced"') && template.includes('confirmAdvanced'))
  const order = ['apps-hero', "formData.name", 'id="apps-cover"', "t('apps.section_launch')", "t('apps.cmd_prep_name')", 'v-if="!showAdvanced"', 'apps-savebar'].map((m) => template.indexOf(m))
  assert.deepEqual([...order].sort((a, b) => a - b), order, 'the order of the groups')
})

test('the expert fields sit behind the advanced section', () => {
  const advanced = template.slice(template.indexOf('v-if="!showAdvanced"'))
  for (const key of ["formData.gamepad", "formData['mouse-mode']", "formData.detached", "formData['rtx-hdr']", 'formData.dlssnr']) assert.ok(advanced.includes(key), key)
  const before = template.slice(0, template.indexOf('v-if="!showAdvanced"'))
  for (const key of ["formData.gamepad", "formData.dlssnr", "formData['rtx-hdr']"]) assert.ok(!before.includes(key), `${key} is not outside the advanced section`)
})

test('sorting the cards by name leaves the saved order alone', () => {
  const cards = ['Steam', 'desktop', 'Zelda 10', 'Zelda 2'].map((title) => ({ title }))
  assert.deepEqual(sortCards(cards, 'custom'), cards)
  assert.deepEqual(sortCards(cards, 'asc').map((c) => c.title), ['desktop', 'Steam', 'Zelda 2', 'Zelda 10'])
  assert.deepEqual(sortCards(cards, 'desc').map((c) => c.title), ['Zelda 10', 'Zelda 2', 'Steam', 'desktop'])
  assert.deepEqual(cards.map((c) => c.title), ['Steam', 'desktop', 'Zelda 10', 'Zelda 2'])
})

test('the editor page: the poster banner with 更换封面, the page and its groups move in with the spring, the advanced groups too, and the Sparxie groups come only with the component', () => {
  assert.ok(template.includes("t('apps.change_cover')") && template.includes('class="apps-hero__edit"'))
  for (const loc of ['en', 'zh']) {
    const messages = JSON.parse(readFileSync(new URL(`../public/assets/locale/${loc}.json`, import.meta.url), 'utf8'))
    assert.ok(messages.apps.change_cover, `${loc} has the label`)
  }
  assert.ok(/@keyframes apps-rise/.test(editor) && /\.apps-editor--page > \* \{[^}]*animation: apps-rise var\(--m-spring-default-spatial-duration/.test(editor))
  assert.ok(/animation: apps-rise[^;]*backwards/.test(editor), 'the animation leaves no transform on the sticky save bar')
  // the Sparxie groups are behind the component-present check; without the component the page is the one above
  assert.ok(!editor.includes('SparxieGame') && !editor.includes('spxApp'), 'a game app is edited on the page 添加游戏; this editor is for every other application')
  // the save bar is a bar of ordinary buttons at the bottom of the page, not a full-width block
  assert.ok(/class="apps-savebar"/.test(template) && !/block/.test(template.slice(template.indexOf('apps-savebar'))))
  assert.ok(!template.slice(template.indexOf('apps-savebar')).includes('apps-pill'), 'the save button is an ordinary button (apps-pill is the 56 px stretch of the scan sheet)')
})

test('a long command example stays inside the card on a narrow screen (it is cut with an ellipsis, not clipped by the edge)', () => {
  const less = readFileSync(new URL('../styles/m3e-web.less', import.meta.url), 'utf8')
  assert.ok(/\.apps-examples__tags \{[^}]*max-width: 100%/.test(less) && /\.apps-examples \{[^}]*min-width: 0/.test(less))
  assert.ok(/\.cmd-tag code \{[^}]*text-overflow: ellipsis/.test(less))
})

test('the client app type of the host card (fs-kind) is still on the editor page and is saved only when set', () => {
  assert.match(template, /v-model="formData\['fs-kind'\]"/)
  assert.match(editor, /if \(!editedApp\['fs-kind'\]\) delete editedApp\['fs-kind'\]/)
})

// ---- portrait AND landscape cover: apps.json `hero` next to `image-path` (the hero of the client interface, /appasset?type=hero) ----
const selector = readFileSync(new URL('../components/ImageSelector.vue', import.meta.url), 'utf8')

test('the cover section has a portrait slot and a landscape slot; either, both or none may be set', () => {
  assert.match(template, /<ImageSelector kind="poster" :title="t\('apps\.cover_portrait'\)" :image-path="formData\['image-path'\]"[^>]*@update-image="updateImage"/)
  assert.match(template, /<ImageSelector kind="hero" :title="t\('apps\.cover_landscape'\)" :image-path="formData\.hero"[^>]*@update-image="updateHero"/)
  assert.match(editor, /const updateHero = \(imagePath\) => \{\n  formData\.value\.hero = imagePath/)
  // an empty hero is not written into apps.json (no key at all); a set one loses stray quotes like the portrait path
  assert.match(editor, /if \(editedApp\.hero\) editedApp\.hero = editedApp\.hero\.toString\(\)\.replace\(\/"\/g, ''\)\n  else delete editedApp\.hero/)
  assert.match(editor, /^  hero: '',$/m)
  // the banner of the page shows the landscape cover first
  assert.match(editor, /formData\.value\?\.hero \|\| formData\.value\?\.\['image-path'\]/)
})

test('each slot has choose-file, drop, clear and a preview frame at the true aspect of its cover, with an empty, a broken and an uploading state', () => {
  assert.match(selector, /<MButton variant="tonal" icon="upload" :loading="uploading" @click="pickFile">\{\{ \$t\('apps\.choose_image'\) \}\}<\/MButton>/)
  assert.match(selector, /<MButton v-if="imagePath" variant="text" icon="close" data-image="clear" @click="\$emit\('update-image', ''\)">\{\{ \$t\('apps\.image_clear'\) \}\}<\/MButton>/)
  assert.match(selector, /@drop\.prevent\.stop="handleDrop"/)
  assert.match(selector, /\.cover-card--poster \.cover-frame \{[^}]*aspect-ratio: 2 \/ 3;/)
  assert.match(selector, /\.cover-card--hero \.cover-frame \{[^}]*aspect-ratio: 16 \/ 9;/)
  assert.match(selector, /cover-frame__choose/)
  assert.match(selector, /showBroken/)
  assert.match(selector, /uploading = true/)
  // the landscape slot is a plain image: no wallpaper switch, no poster finder, its own upload key
  assert.match(selector, /<MSwitchItem v-if="kind === 'poster'"/)
  assert.match(selector, /<CoverFinder\n      v-if="kind === 'poster'"/)
  assert.match(selector, /const prefix = this\.kind === 'hero' \? 'app_hero' : 'app'/)
})

test('the titles of the two slots exist in English and Chinese', () => {
  for (const lang of ['en', 'zh']) {
    const apps = JSON.parse(readFileSync(new URL(`../public/assets/locale/${lang}.json`, import.meta.url), 'utf8')).apps
    for (const key of ['cover_portrait', 'cover_landscape', 'image_clear', 'choose_image']) assert.ok(apps[key], `${lang} apps.${key}`)
  }
})
