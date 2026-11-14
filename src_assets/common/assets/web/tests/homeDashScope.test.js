import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// The overview gave MHomeDash neither the host configuration nor the language, so a pinned setting tile showed its default state in English whatever the host had set. The pinned setting tiles read config and speak locale.
test("the overview gives the dashboard the host's configuration and the language (the pinned setting tiles read them)", () => {
  const source = fs.readFileSync(new URL('../views/Home.vue', import.meta.url), 'utf8')
  const tag = /<MHomeDash\b[^>]*>/s.exec(source)?.[0] || ''
  assert.match(tag, /:config="hostConfig"/)
  assert.match(tag, /:locale="locale"/)
})
