import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

const read = (p) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8')

test('a lazy pane that names no loading state gets the page skeleton', () => {
  const src = read('utils/lazyPane.js')
  assert.match(src, /MPageLoading/)
  assert.match(src, /loading = DefaultLoading/)
  assert.doesNotMatch(src, /loading \|\| undefined/)
})

test('the five encoder panes and the app editor load through lazyPane with a loading state', () => {
  const enc = read('configs/tabs/display/DisplayEncoding.vue')
  assert.doesNotMatch(enc, /defineAsyncComponent/)
  assert.equal((enc.match(/= encoderChunk\(\(\) => import\(/g) || []).length, 5)
  const apps = read('views/Apps.vue')
  assert.doesNotMatch(apps, /defineAsyncComponent/)
  assert.match(apps, /lazyPane\(\(\) => import\('\.\.\/components\/AppEditor\.vue'\), \{ loading: AppEditorLoading \}\)/)
  const loading = read('components/AppEditorLoading.vue')
  assert.match(loading, /<MPage[^>]*back/) // the editor is a page of the area, not a side sheet
  assert.match(loading, /MPageLoading/)
})

test('the clients page stays on its list skeleton until the first list is back, and the log page draws the log as a skeleton while it has none', () => {
  const pin = read('views/Pin.vue')
  assert.match(pin, /v-if="\(loading \|\| !listed\) && !clients\.length"/)
  assert.match(pin, /const clientsLoaded = await refreshClients\(\)\n  listed\.value = true/)
  assert.match(read('views/Troubleshooting.vue'), /:log-loading="logs === 'Loading\.\.\.'"/)
})
