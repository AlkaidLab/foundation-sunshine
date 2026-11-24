// SPDX-License-Identifier: GPL-3.0-only
// The settings search against this product's real data: the cases of what people typed and did not find (helpers/searchEvalCases.mjs), the aliases of the settings,
// the pinyin table, and the navigation bar as a search target.
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { CASES, SUGGESTIONS } from './helpers/searchEvalCases.mjs'
import { runEval } from './helpers/searchEvalRun.mjs'
import { NAV_PAGES, NAV_RAILS, SEARCH_INDEX, buildIndex, search, searchDocsOf } from './helpers/searchDocs.mjs'
const { ALIASES } = await import('../config/settingsData/aliases.js')
const { uncoveredHan } = await import('foundation-ui/search/pinyin.js')

test('the evaluation set: the answer is among the first three results for every case', () => {
  const r = runEval()
  const misses = r.rows.filter((x) => !x.skipped && !x.hit)
  console.log(`  search evaluation: ${r.hits}/${r.total} cases have the answer in the first three (${(100 * r.hits / r.total).toFixed(1)} %), ${r.firsts} as the first; ${r.skipped} skipped (docs not in this build)`)
  assert.deepEqual(misses.map((x) => `${x.locale} "${x.query}" -> ${x.got.join(', ')}`), [])
  assert.ok(r.total >= 100, `${r.total} cases`)
  // only the cases about the reset page may be skipped: it arrives with its own card; any other skip means a doc id was misspelt
  assert.ok(r.rows.filter((x) => x.skipped).every((x) => CASES.find((c) => c[0] === x.locale && c[1] === x.query)[2].every((id) => id === 'action:resetSettings')), 'only reset cases are skipped')
})

test('did you mean: a query that matches nothing offers the nearest settings and pages', () => {
  const r = runEval()
  for (const x of r.suggest) assert.ok(x.hit, `${x.locale} "${x.query}" -> ${x.found ? 'results' : x.offered.join(', ')}`)
  assert.ok(SUGGESTIONS.length >= 3)
})

test('aliases: each belongs to a setting of the index, is a short list of words, and nothing is listed twice', () => {
  const ids = new Set(SEARCH_INDEX.map((e) => e.id))
  for (const [id, list] of Object.entries(ALIASES)) {
    assert.ok(ids.has(id), `${id} is not a setting of the index`)
    assert.ok(Array.isArray(list) && list.length >= 1 && list.length <= 14, `${id}: ${list.length} aliases`)
    assert.equal(new Set(list).size, list.length, `${id} lists a word twice`)
    for (const w of list) assert.ok(typeof w === 'string' && w.trim() === w && w.length >= 2 && w === w.toLowerCase(), `${id}: "${w}"`)
  }
  // the settings people look for most have aliases
  for (const id of ['stream_audio', 'audio_sink', 'virtual_sink', 'stream_mic', 'resolution_change', 'max_bitrate', 'controller', 'external_endpoints', 'locale']) assert.ok(ALIASES[id], id)
})

test('every Han character of the search docs has its pinyin in foundation-ui (a character without it only loses the whole-word pinyin: its initial still works)', () => {
  const text = [...searchDocsOf('zh'), ...searchDocsOf('en')].map((d) => `${d.title}\n${d.path}\n${d.keywords}\n${[].concat(d.aliases || []).join(' ')}`).join('\n')
  const all = new Set([...text].filter((c) => c >= '一' && c <= '鿿'))
  const missing = uncoveredHan(text)
  assert.ok(missing.length / all.size < 0.03, `${missing.length} of ${all.size} characters lack pinyin: ${missing.join('')}; run foundation-ui's scripts/gen-pinyin.mjs`)
})

test('the navigation bar is searchable, and the mirror of the shell used here still matches the shell', () => {
  const shell = readFileSync(new URL('../views/WebShell.vue', import.meta.url), 'utf8')
  for (const [id, [rail, key]] of Object.entries(NAV_PAGES)) assert.ok(new RegExp(`['"]?${id}['"]?: \\{ rail: '${rail}'[^}]*label: '${key.replace(/\./g, '\\.')}'`).test(shell), `${id} in WebShell.vue`)
  for (const [id, icon, key] of NAV_RAILS) assert.ok(shell.includes(`{ id: '${id}', icon: '${icon}', label: '${key}' }`), `rail ${id} in WebShell.vue`)
  assert.match(shell, /railDocsOf/)
  const index = buildIndex(searchDocsOf('zh'))
  for (const [word, rail] of [['音频', 'audio'], ['显示与视频', 'display'], ['输入与外设', 'input'], ['系统', 'system'], ['概览', 'overview'], ['yinpin', 'audio']]) {
    assert.ok(search(index, word).flat.slice(0, 4).some((d) => d.id === `page:rail-${rail}`), `${word} finds the bar's ${rail}`)
  }
  assert.ok(search(index, '声音').flat.some((d) => d.id === 'page:rail-audio'), '声音 reaches the bar\'s audio too')
  const en = buildIndex(searchDocsOf('en'))
  assert.ok(search(en, 'audio').flat.slice(0, 3).some((d) => d.id === 'page:rail-audio') && search(en, 'display').flat.slice(0, 7).some((d) => d.id === 'page:rail-display'))
})

test('what was found before is still found: every setting by its label and by its key, in both languages, among the first three', () => {
  for (const locale of ['zh', 'en']) {
    const docs = searchDocsOf(locale)
    const index = buildIndex(docs)
    const lost = []
    for (const d of docs.filter((x) => x.type === 'setting')) {
      for (const q of [d.title, d.key]) if (!search(index, q).flat.slice(0, 3).some((x) => x.id === d.id)) lost.push(`${locale} ${q}`)
    }
    assert.deepEqual(lost, [])
  }
})

test('a keystroke over the real index of this product stays far under a frame', () => {
  const index = buildIndex(searchDocsOf('zh'))
  const queries = ['声', '声音', 'yinpin', 'yp', 'sepaker', 'mic', '没有声音', '麦克风没声音', 'bitr', 'hevc_mode', 'zzzzqq', '启用音频', 'second monitor', '主机没有显示器时自动创建基地虚拟显示器']
  for (const q of queries) search(index, q)
  const times = []
  for (let round = 0; round < 20; round++) for (const q of queries) { const t = performance.now(); search(index, q); times.push(performance.now() - t) }
  times.sort((a, b) => a - b)
  const p95 = times[Math.floor(times.length * 0.95)]
  console.log(`  ${index.size} docs: keystroke median ${times[times.length >> 1].toFixed(3)} ms, p95 ${p95.toFixed(3)} ms, worst ${times.at(-1).toFixed(3)} ms`)
  assert.ok(p95 < 5, `p95 ${p95.toFixed(2)} ms`)
})
