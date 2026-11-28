import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import {
  COVER_STYLES, COVER_STYLE_AUTO, appCoverChoice, coverAppId, coverEffectiveStyle, coverPreviewUrl, coverSourceOf, isPlaceholderImagePath, normalizeCoverStyle, withoutCoverReport,
} from '../utils/generatedCover.js'

const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')

test('the six styles are the ones the host accepts, and an unknown value is the default', () => {
  assert.deepEqual(COVER_STYLES, ['tonal', 'monogram', 'icon', 'editorial', 'glass', 'geometric'])
  assert.equal(normalizeCoverStyle('glass'), 'glass')
  assert.equal(normalizeCoverStyle('nope'), 'tonal')
  assert.equal(normalizeCoverStyle(undefined), 'tonal')
})

test('an application follows the global style unless it carries one of the six', () => {
  assert.equal(appCoverChoice({}), COVER_STYLE_AUTO)
  assert.equal(appCoverChoice({ 'cover-style': 'auto' }), COVER_STYLE_AUTO)
  assert.equal(appCoverChoice({ 'cover-style': 'editorial' }), 'editorial')
  assert.equal(appCoverChoice({ 'cover-style': 'bogus' }), COVER_STYLE_AUTO)
})

test('the host id is read from the cover URL, and the preview address carries the saved name', () => {
  const app = { 'cover-portrait-url': '/appasset?appid=42&type=poster' }
  assert.equal(coverAppId(app), '42')
  assert.equal(coverAppId({ cover: { portrait: '/appasset?appid=7&type=poster' } }), '7')
  assert.equal(coverAppId({}), '')
  const url = coverPreviewUrl('42', 'glass', 'landscape', '原神 (CN)')
  const parsed = new URL(url, 'http://host.invalid')
  assert.equal(parsed.pathname, '/api/apps/cover-preview')
  assert.equal(parsed.searchParams.get('appid'), '42')
  assert.equal(parsed.searchParams.get('style'), 'glass')
  assert.equal(parsed.searchParams.get('orientation'), 'landscape')
  assert.equal(parsed.searchParams.get('n'), '原神 (CN)')
  assert.equal(coverPreviewUrl('', 'glass'), '')
  assert.equal(coverPreviewUrl('42', 'nope'), '')
})

test('the source and the style the host reports are read from either shape of the answer', () => {
  assert.equal(coverSourceOf({ 'cover-source': 'generated' }), 'generated')
  assert.equal(coverSourceOf({ cover: { source: 'xbox' } }), 'xbox')
  assert.equal(coverSourceOf({ 'cover-source': 'other' }), '')
  assert.equal(coverEffectiveStyle({ 'cover-effective-style': 'icon' }), 'icon')
  assert.equal(coverEffectiveStyle({ cover: { style: 'glass' } }), 'glass')
  assert.equal(coverEffectiveStyle({}), '')
})

test('the fields that describe the cover are not sent back with the application', () => {
  const sent = withoutCoverReport({ name: 'A', 'cover-style': 'glass', 'cover-source': 'generated', 'cover-effective-style': 'glass', 'cover-portrait-url': 'x', 'cover-landscape-url': 'y', cover: {} })
  assert.deepEqual(sent, { name: 'A', 'cover-style': 'glass' })
})

test('every language has the texts of the cover strip and its status line', () => {
  const dir = join(WEB, 'public/assets/locale')
  const apps = ['cover_style_title', 'cover_style_auto', ...COVER_STYLES.map((style) => `cover_style_${style}`), 'cover_use_generated', 'cover_status_generated', 'cover_status_following', 'cover_status_poster', 'cover_global_title', 'cover_global_save_failed']
  const files = readdirSync(dir).filter((f) => f.endsWith('.json'))
  assert.ok(files.length >= 20)
  for (const file of files) {
    const data = JSON.parse(readFileSync(join(dir, file), 'utf8'))
    for (const key of apps) assert.ok(typeof data.apps?.[key] === 'string' && data.apps[key].length > 0, `${file}: apps.${key}`)
    assert.ok(data.apps.cover_status_generated.includes('{style}') && data.apps.cover_status_following.includes('{style}'), `${file}: the style placeholder`)
    assert.equal(data.config?.generated_cover_style, undefined, `${file}: the style is not a settings page text`)
  }
})

test('one cover section: each preview shows the cover in force with its own strip, and the global style is set above the list of applications', () => {
  const editor = readFileSync(join(WEB, 'components/AppEditor.vue'), 'utf8')
  const selector = readFileSync(join(WEB, 'components/ImageSelector.vue'), 'utf8')
  const apps = readFileSync(join(WEB, 'views/Apps.vue'), 'utf8')
  assert.match(editor, /withoutCoverReport\(formData\.value\)/)
  assert.ok(!/AppCoverStyle|CoverStylePicker/.test(editor), 'the cover is one section: no second block of styles')
  assert.match(editor, /:generated-src="posterGeneratedSrc"/)
  assert.match(editor, /:generated-src="heroGeneratedSrc"/)
  // a generated cover is the preview whenever the host draws one: never an empty frame beside it
  assert.match(selector, /showGenerated\(\) \{\n      return !this\.ownPath && !!this\.generatedSrc/)
  assert.match(selector, /<CoverStyleStrip\n\s+v-if="showStrip && showGenerated"/)
  assert.match(selector, /data-image="use-generated"/)
  assert.match(apps, /<CoverStyleStrip :model-value="globalCoverStyle"/)
  assert.match(apps, /async function saveGlobalCoverStyle/)
  assert.ok(!/generated_cover_style/.test(readFileSync(join(WEB, 'configs/tabs/General.vue'), 'utf8')), 'the global style is not a setting of the settings pages')
})

test('the placeholder picture is told by every name the default Xbox entry and older versions wrote it with, a picture of the person is not', () => {
  for (const path of ['', undefined, null, 'box.png', 'BOX.PNG', './assets/box.png', '.\\assets\\box.png', '"box.png"', ' assets/box.png ']) {
    assert.equal(isPlaceholderImagePath(path), true, String(path))
  }
  for (const path of ['poster.png', 'D:\\art\\box.png', '/home/me/box.png', 'steam.png', 'desktop', 'covers/box.png']) {
    assert.equal(isPlaceholderImagePath(path), false, path)
  }
})
