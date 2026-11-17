import './helpers/foundationUi.mjs'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import test from 'node:test'
import { CLIENT_RESOURCES, COMMUNITY_RESOURCES } from '../config/resources.js'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('AB3: every client row of the resource sheet is tagged official or partner, the official ones being the four of the team', () => {
  const rows = [...CLIENT_RESOURCES, ...COMMUNITY_RESOURCES]
  assert.ok(rows.every((r) => r.client === 'official' || r.client === 'partner'), 'every client row has a tag')
  assert.deepEqual(rows.filter((r) => r.client === 'official').map((r) => r.id).sort(), ['android-vplus', 'harmony-vplus', 'moonlight-desktop', 'moonlight-macos'])
  assert.deepEqual(rows.filter((r) => r.client === 'partner').map((r) => r.id).sort(), ['crown-edition', 'moonlight-ohos', 'voidlink'])
  assert.match(read('../components/common/ResourceSheet.vue'), /<MClientTag v-if="resource\.client" :official="resource\.client === 'official'"/)
})

test('AB3: the download page tags every client card and the popup of a client with the same chip', () => {
  const page = read('../views/Downloads.vue')
  assert.match(page, /<MClientTag :official="!card\.partner" :locale="locale" \/>/)
  assert.match(page, /<MClientTag :official="!dialogCard\.partner" :locale="locale" \/>/)
  assert.ok(!/dl-tag--partner/.test(page + read('../styles/m3e-web.less')), 'the old partner tag is gone')
})

test('follow-up: the platform chips beside the tag are plain facts (neutral), not a second tonal pill', () => {
  const less = read('../styles/m3e-web.less')
  assert.match(less, /\.dl-tag \{[^}]*background: var\(--md-sys-color-surface-container-high\); color: var\(--md-sys-color-on-surface-variant\)/)
})

test('the box of a card\'s tags reserves room but its tags keep their own height: a wrapped line is not stretched into a tall pill', () => {
  const less = read('../styles/m3e-web.less')
  const rule = less.match(/\.dl-card__tags \{ display: flex; flex-wrap: wrap;[^}]*\}/)
  assert.ok(rule, 'the tag box rule exists')
  assert.match(rule[0], /align-items: flex-start; align-content: flex-start;/, 'flex-start, not the default stretch')
  assert.match(read('../views/Downloads.vue'), /const tagsReserve = \(card\) => \(card\.client\.product \? \{ minHeight:/, 'the reserve is what made the line stretch')
})

test('a long system tag wraps inside its chip (no ellipsis cut of "Universal")', () => {
  const less = read('../styles/m3e-web.less')
  assert.match(less, /\.dl-card__tags \.dl-tag \{ max-width: 100%; min-width: 0; overflow-wrap: anywhere; border-radius: 12px; \}/)
  assert.doesNotMatch(less, /\.dl-card__tags \.dl-tag \{[^}]*text-overflow: ellipsis/)
})
