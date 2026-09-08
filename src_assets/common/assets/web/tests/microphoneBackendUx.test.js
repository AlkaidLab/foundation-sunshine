import assert from 'node:assert/strict'
import { readdir, readFile } from 'node:fs/promises'
import test from 'node:test'

const localeDirectory = new URL('../public/assets/locale/', import.meta.url)
const recommendationPatterns = {
  bg: /препоръч/i,
  cs: /doporuč/i,
  de: /empfohl/i,
  en: /recommend/i,
  en_GB: /recommend/i,
  en_US: /recommend/i,
  es: /recomendad/i,
  fr: /recommand/i,
  it: /consigliat|raccomandat/i,
  ja: /推奨/,
  ko: /권장/,
  pl: /zalecan/i,
  pt: /recomendad/i,
  pt_BR: /recomendad/i,
  ru: /рекоменд/i,
  sv: /rekommend/i,
  tr: /öneril|tavsiye/i,
  uk: /рекоменд/i,
  zh: /推荐/,
  zh_TW: /建議|推薦/,
}

test('experimental USB/IP routing is not labelled as recommended', async () => {
  const localeFiles = (await readdir(localeDirectory)).filter((name) => name.endsWith('.json'))

  for (const localeFile of localeFiles) {
    const locale = JSON.parse(await readFile(new URL(localeFile, localeDirectory), 'utf8'))
    const label = locale.config?.microphone_redirect_backend_auto
    const language = localeFile.replace(/\.json$/, '')
    const recommendationPattern = recommendationPatterns[language]

    assert.equal(typeof label, 'string', `${localeFile} is missing the automatic microphone backend label`)
    assert.ok(recommendationPattern, `${localeFile} is missing a recommendation-language guard`)
    assert.doesNotMatch(
      label,
      recommendationPattern,
      `${localeFile} recommends an experimental backend`,
    )
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
