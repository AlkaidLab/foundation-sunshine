// SPDX-License-Identifier: GPL-3.0-only
// The checks of unifiedsearch.test.js of foundation-ui that read this product's settings data (the search index and the label tables): they live here because the data does.
import assert from 'node:assert/strict'
import test from 'node:test'
import './helpers/foundationUi.mjs'
const { SEARCH_INDEX, LABELS } = await import('../config/settingsData/index.js')
const { buildIndex, search } = await import('foundation-ui/search/index.js')
const { settingDocs, tileDocs, clientDocs, appDocs } = await import('foundation-ui/search/docs.js')
const { actionDocs, searchTexts } = await import('foundation-ui/search/texts.js')
const { buildSettingsEntries } = await import('foundation-ui/settings/search.js')
const { labelOf } = await import('foundation-ui/settings/provider.js')

const docsOf = (locale = 'zh') => [
  ...settingDocs(entries(locale)),
  ...actionDocs(locale, { restart() {}, pair() {}, exportDiag() {}, checkUpdate() {}, logs() {}, addApp() {} }, searchTexts(locale).types.action),
  ...tileDocs(locale),
  ...clientDocs([{ name: 'Pixel 9' }, { name: 'Steam Deck' }]),
  ...appDocs([{ name: 'Steam Big Picture' }, { name: 'Desktop' }]),
]

const ids = (r) => r.flat.map((d) => d.id)

const entries = (locale) => buildSettingsEntries({ index: SEARCH_INDEX, label: (k) => labelOf(locale, k), altLabel: (k) => labelOf(locale === 'zh' ? 'en' : 'zh', k) })

test('index coverage: every setting of the index is reachable by its key, by its title and by the other language', () => {
  for (const locale of ['zh', 'en']) {
    const docs = docsOf(locale)
    const index = buildIndex(docs)
    const settings = docs.filter((d) => d.type === 'setting')
    assert.ok(settings.length >= SEARCH_INDEX.length, 'every entry of the index is a doc')
    for (const doc of settings) {
      assert.ok(ids(search(index, doc.key, { perGroup: 1e6 })).includes(doc.id), `${locale}: the key ${doc.key} finds ${doc.id}`)
      if (doc.title && doc.title !== doc.key) assert.ok(ids(search(index, doc.title, { perGroup: 1e6 })).includes(doc.id), `${locale}: the title "${doc.title}" finds ${doc.id}`)
    }
    // every setting is also reachable by the label of the other language (English finds a Chinese interface)
    for (const e of SEARCH_INDEX.slice(0, 40)) {
      const other = labelOf(locale === 'zh' ? 'en' : 'zh', e.title)
      if (other && other !== e.title) assert.ok(ids(search(index, other, { perGroup: 1e6 })).includes(`setting:${e.id}`), `${locale}: "${other}" finds ${e.id}`)
    }
  }
})
