import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('the settings page never shows the title of the tab or of the section before while a section opens, and its body has a loading state', () => {
  const cfg = read('../views/Config.vue')
  assert.ok(/<template #fallback><MSkeleton/.test(cfg), 'the content area shows a skeleton while the tab chunk loads')
  assert.ok(/if \(subNav\.current\.value\) return subTitle\.value/.test(cfg), 'no tab title while the section has not said its name')
  assert.ok(/subTitle\.value = ''/.test(cfg), 'the name of the section before is dropped when another opens')
})
