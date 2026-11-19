import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// Every page of the Web UI is a document of its own (the panel moves its frame between them): its first render must not wait for the host. The language comes from the kept answer.
const src = fs.readFileSync(new URL('../config/i18n.js', import.meta.url), 'utf8')

test('the language is read from the kept answer at once and revalidated in the background; only a first start waits for the host', () => {
  assert.match(src, /const kept = hostData\.peek\('config'\);\s*if \(kept && kept\.locale\) \{\s*locale = kept\.locale;\s*revalidate = hostData\.refresh\('config'\)/)
  assert.match(src, /\} else \{\s*try \{\s*const config = await hostData\.refresh\('config'\);/, 'no kept answer: wait, as before')
  assert.match(src, /if \(revalidate\) revalidate\.then\(\(fresh\) => \{ if \(fresh && fresh !== hostLocale && !pref\) applyLocale\(fresh\); \}\);/)
})
