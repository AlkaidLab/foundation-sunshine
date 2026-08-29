import assert from 'node:assert/strict'
import { readdir, readFile } from 'node:fs/promises'
import test from 'node:test'

const localeDirectory = new URL('../public/assets/locale/', import.meta.url)

test('experimental USB/IP routing is not labelled as recommended', async () => {
  const localeFiles = (await readdir(localeDirectory)).filter((name) => name.endsWith('.json'))

  for (const localeFile of localeFiles) {
    const locale = JSON.parse(await readFile(new URL(localeFile, localeDirectory), 'utf8'))
    const label = locale.config?.microphone_redirect_backend_auto

    assert.equal(typeof label, 'string', `${localeFile} is missing the automatic microphone backend label`)
    assert.doesNotMatch(label, /recommended|推荐|建議/i, `${localeFile} recommends an experimental backend`)
  }
})
