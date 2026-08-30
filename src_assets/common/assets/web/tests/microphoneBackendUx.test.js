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

test('reviewed microphone locales do not fall back to English', async () => {
  const english = JSON.parse(await readFile(new URL('en.json', localeDirectory), 'utf8'))
  const microphoneKeys = [
    ...Object.keys(english.config).filter((key) => key.startsWith('microphone_redirect_')),
    'stream_mic_test_note_usbip',
    'stream_mic_test_success_usbip',
    'stream_mic_test_usbip_unavailable',
  ]

  for (const language of ['es', 'fr', 'it', 'ja', 'ko', 'tr', 'uk']) {
    const locale = JSON.parse(
      await readFile(new URL(`${language}.json`, localeDirectory), 'utf8'),
    )

    for (const key of microphoneKeys) {
      assert.equal(typeof locale.config?.[key], 'string', `${language}.json is missing ${key}`)
      assert.notEqual(
        locale.config[key],
        english.config[key],
        `${language}.json leaves ${key} in English`,
      )
    }
  }
})
