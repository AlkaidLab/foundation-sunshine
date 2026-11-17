import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"

const page = readFileSync(new URL("../views/Downloads.vue", import.meta.url), "utf8")
const less = readFileSync(new URL("../styles/m3e-web.less", import.meta.url), "utf8")

test("the list without a filter is a grid of client cards; the lists of one system keep their own cards", () => {
  assert.match(page, /<div v-if="osFilter === .all." class="m-cgrid">/)
  assert.match(page, /<MClientCard /)
  assert.match(page, /<div v-else class="dl-grid">/)
})

test("a card downloads through the page\x27s own link opener, so a download starts the way the popup\x27s does", () => {
  assert.match(page, /@open-url="openLink"/)
  assert.match(page, /@open="openDialog\(card\)"/)
})

test("only the unfiltered page uses the wide column", () => {
  assert.match(page, /\x27dl-page--all\x27: osFilter === \x27all\x27/)
  assert.match(less, /\.dl-page--all \.m-page \{ --m-column: min\(100%, 1200px\); \}/)
})
