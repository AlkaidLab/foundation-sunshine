import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, readdirSync } from 'node:fs'
import { ICONS } from 'foundation-ui/icons'
import { CLIENT_FEATURE_ICONS, CLIENT_PENDING_TAGS } from '../config/clientDownloads.js'

// The feature tags of Moonlight macOS Enhanced (popup of 下载客户端). Goal: nobody reads the page and thinks Moonlight V+ has something Enhanced has not. So the first six are the tags the V+ desktop
// clients show (same words in every language, same icons), then two that Enhanced does best on macOS: eight in all, no more. Every tag is something the PUBLISHED release v1.3.9-rc.7 has (README,
// release notes or code at that tag). What only the branch feat/enhanced-full has is in CLIENT_PENDING_TAGS and not shown.
const dir = new URL('../i18n/downloads/', import.meta.url)
const locales = readdirSync(dir).filter((f) => f.endsWith('.json')).map((f) => [f.slice(0, -5), JSON.parse(readFileSync(new URL(f, dir), 'utf8'))])

test('macOS Enhanced shows eight tags with one icon and one caption each, in every language', () => {
  const icons = CLIENT_FEATURE_ICONS['macos-enhanced']
  assert.equal(icons.length, 8)
  for (const [loc, j] of locales) {
    const p = j.platforms['macos-enhanced']
    assert.equal(p.features.length, 8, `${loc}: tags`)
    assert.equal(p.captions.length, 8, `${loc}: captions`)
    for (const tag of p.features) assert.ok(tag.length <= 40, `${loc}: "${tag}" is short`)
  }
  for (const icon of icons) assert.ok(ICONS[icon], `${icon} is a glyph of the set`)
})

test('every tag V+ shows on the desktop is also an Enhanced tag, in the same words and with the same icon', () => {
  const E = CLIENT_FEATURE_ICONS['macos-enhanced']
  const cases = [['macos', 0], ['macos', 1], ['macos', 2], ['windows', 2], ['macos', 3], ['linux', 2]] // HDR video, keyboard and controllers, two-way clipboard, remote microphone, shared host files, in-stream controls
  for (const [loc, j] of locales) {
    const mine = j.platforms['macos-enhanced'].features
    cases.forEach(([platform, i], k) => {
      assert.equal(mine[k], j.platforms[platform].features[i], `${loc}: tag ${k + 1} is the V+ wording`)
      assert.equal(E[k], CLIENT_FEATURE_ICONS[platform][i], `tag ${k + 1} has the V+ icon`)
    })
  }
})

test('the two extras are DualSense and Mac input; ja and ko are translated, the rest fall back to English', () => {
  const get = (l) => locales.find(([x]) => x === l)[1].platforms['macos-enhanced']
  assert.equal(get('en').features[6], 'DualSense haptics, triggers and motion')
  assert.equal(get('en').features[7], 'Mac input and trackpad')
  for (const l of ['zh', 'zh_TW', 'ja', 'ko']) assert.ok(get(l).features[6].startsWith('DualSense') && get(l).features[6] !== get('en').features[6], `${l} translated`)
  assert.match(get('ja').captions[6], /[぀-ヿ]/)
  assert.match(get('ko').captions[6], /[가-힣]/)
})

test('no claim the published release does not back: what only the branch has is kept in code and not shown', () => {
  const pending = CLIENT_PENDING_TAGS['macos-enhanced']
  assert.deepEqual(pending.map((x) => x.id), ['ds-player-leds', 'ds-speaker-mic', 'ds-bluetooth-haptics', 'ds-lightbar-brightness'])
  for (const x of pending) assert.ok(x.evidence && x.en && x.zh && ICONS[x.icon], x.id)
  for (const [loc, j] of locales) {
    const p = j.platforms['macos-enhanced']
    const shown = [...p.features, ...p.captions].join('|')
    for (const x of pending) assert.ok(!shown.includes(x.en) && !shown.includes(x.zh), `${loc}: ${x.id} is not shown`)
    assert.ok(!/player LEDs|speaker and mic|Bluetooth haptics|brightness/i.test(shown), `${loc}: no unreleased claim`)
  }
})

test('the tags of the other clients are not changed (V+ keeps its four)', () => {
  for (const id of ['android', 'windows', 'linux', 'macos']) {
    for (const [loc, j] of locales) assert.equal(j.platforms[id].features.length, CLIENT_FEATURE_ICONS[id].length, `${loc}/${id}`)
  }
  assert.equal(CLIENT_FEATURE_ICONS.android.length, 4)
  assert.equal(CLIENT_FEATURE_ICONS.windows.length, 4)
})
