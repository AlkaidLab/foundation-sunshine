// SPDX-License-Identifier: GPL-3.0-only
// Runs the cases of searchEvalCases.mjs against the search of foundation-ui: the share of the cases whose answer is among the first three results (and the first).
import { CASES, SUGGESTIONS } from './searchEvalCases.mjs'
import { buildIndex, search, searchDocsOf } from './searchDocs.mjs'

export function runEval({ top = 3 } = {}) {
  const indexes = {}
  const docIds = {}
  for (const locale of ['zh', 'en']) { const docs = searchDocsOf(locale); indexes[locale] = buildIndex(docs); docIds[locale] = new Set(docs.map((d) => d.id)) }
  const rows = []
  for (const [locale, query, any] of CASES) {
    const present = any.filter((id) => docIds[locale].has(id))
    if (!present.length) { rows.push({ locale, query, skipped: true }); continue }
    const flat = search(indexes[locale], query).flat.map((d) => d.id)
    const at = flat.findIndex((id) => present.includes(id))
    rows.push({ locale, query, at, hit: at >= 0 && at < top, first: at === 0, got: flat.slice(0, top) })
  }
  const run = rows.filter((r) => !r.skipped)
  const suggest = SUGGESTIONS.map(([locale, query, any]) => {
    const r = search(indexes[locale], query)
    const offered = (r.suggest || []).map((x) => x.doc.id)
    return { locale, query, found: r.total > 0, offered, hit: r.total > 0 || offered.some((id) => any.includes(id)) }
  })
  return { rows, total: run.length, hits: run.filter((r) => r.hit).length, firsts: run.filter((r) => r.first).length, skipped: rows.length - run.length, suggest }
}
