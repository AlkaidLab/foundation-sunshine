import test from 'node:test'
import assert from 'node:assert/strict'
import { fileURLToPath } from 'node:url'
import { join } from 'node:path'
import { scanFile, listFiles } from 'foundation-ui/testing/copy-lint'

// docs/copy-style.zh.md (foundation-ui): the explaining voice of the interface text, in Chinese and English. The brand voice (greetings, 回来啦, Boom!) is deliberate and is not checked.
const web = fileURLToPath(new URL('../', import.meta.url))
const locale = (code) => join(web, 'public/assets/locale', `${code}.json`)

test('no Web UI text breaks the copy rules', () => {
  const files = ['zh', 'zh_TW', 'en', 'en_GB', 'en_US'].map(locale)
  files.push(join(web, 'i18n/home.js'), join(web, 'i18n/downloads/zh.json'), join(web, 'i18n/downloads/zh_TW.json'), join(web, 'i18n/downloads/en.json'))
  for (const dir of ['views', 'components', 'configs']) files.push(...listFiles(join(web, dir), ['.vue']))
  const hits = []
  for (const file of files) for (const h of scanFile(file)) hits.push(`${file.replace(web, '')}: [${h.rule}] ${h.text.slice(0, 120)}`)
  assert.deepEqual(hits, [], `copy rule violations:\n${hits.join('\n')}`)
})
