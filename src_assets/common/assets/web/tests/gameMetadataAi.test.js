import test from 'node:test'
import assert from 'node:assert/strict'

import { assessCoverMatch, calibrateCoverConfidence } from '../utils/coverSelectionAi.js'
import { enhanceScannedGameNames, getCoverSearchCandidates } from '../utils/gameMetadataAi.js'
import { __aiCacheTestUtils } from '../utils/aiCache.js'

test('getCoverSearchCandidates orders AI terms before fallback names', () => {
  const app = {
    name: 'CP2077.exe',
    'original-name': 'CP2077.exe',
    'canonical-name': 'Cyberpunk 2077',
    'cover-search-terms': ['Cyberpunk 2077', '赛博朋克 2077'],
  }

  assert.deepEqual(getCoverSearchCandidates(app), [
    'Cyberpunk 2077',
    '赛博朋克 2077',
    'CP2077.exe',
  ])
})

test('assessCoverMatch rewards exact search-term cover evidence', () => {
  const app = {
    name: '赛博朋克 2077',
    'canonical-name': 'Cyberpunk 2077',
    'cover-search-terms': ['Cyberpunk 2077', '赛博朋克 2077'],
  }

  const evidence = assessCoverMatch(app, {
    name: 'Cyberpunk 2077',
    source: 'igdb',
    searchTerm: 'Cyberpunk 2077',
  })

  assert.equal(evidence.relation, 'exact-title')
  assert.ok(evidence.confidence >= 0.9)
})

test('assessCoverMatch keeps empty candidate names at source-prior confidence', () => {
  const evidence = assessCoverMatch(
    {
      name: 'Portal 2',
      'canonical-name': 'Portal 2',
      'cover-search-terms': ['Portal 2'],
    },
    {
      name: '',
      source: 'igdb',
      searchTerm: 'Portal 2',
    }
  )

  assert.equal(evidence.relation, 'source-prior')
  assert.equal(evidence.confidence, 0.42)
})

test('calibrateCoverConfidence caps high AI confidence when local evidence is weak', () => {
  const selected = calibrateCoverConfidence(
    {
      name: 'Portal 2',
      'canonical-name': 'Portal 2',
      'cover-search-terms': ['Portal 2'],
    },
    {
      name: 'Portal Knights',
      source: 'steam',
      searchTerm: 'Portal 2',
    },
    0.95,
    'Looks related'
  )

  assert.ok(selected.aiCoverConfidence < 0.8)
  assert.ok(selected.coverMatchConfidence < 0.7)
  assert.equal(selected.aiCoverReason, 'Looks related')
})

test('enhanceScannedGameNames renames only high-confidence results', async () => {
  __aiCacheTestUtils.clearMemoryStores()
  const originalFetch = globalThis.fetch
  const originalDocument = globalThis.document
  let requestBody = null
  let fetchCalls = 0

  globalThis.document = {
    documentElement: {
      getAttribute: (name) => (name === 'lang' ? 'zh-CN' : null),
    },
  }

  globalThis.fetch = async (_url, options = {}) => {
    fetchCalls += 1
    requestBody = JSON.parse(options.body)
    return {
      ok: true,
      async json() {
        return {
          choices: [
            {
              message: {
                content: JSON.stringify({
                  items: [
                    {
                      id: '0',
                      displayName: 'Cyberpunk 2077',
                      canonicalName: 'Cyberpunk 2077',
                      searchTerms: ['Cyberpunk 2077'],
                      isGame: true,
                      confidence: 0.95,
                    },
                    {
                      id: '1',
                      displayName: 'Unknown Game',
                      canonicalName: 'Unknown Game',
                      searchTerms: ['Unknown Game'],
                      isGame: true,
                      confidence: 0.4,
                    },
                  ],
                }),
              },
            },
          ],
        }
      },
    }
  }

  try {
    const result = await enhanceScannedGameNames([
      { name: 'CP2077.exe', cmd: 'C:/Games/Cyberpunk2077/bin/x64/Cyberpunk2077.exe' },
      { name: 'weird.exe', cmd: 'C:/Games/weird.exe' },
    ])

    assert.equal(result[0].name, 'Cyberpunk 2077')
    assert.equal(result[0]['original-name'], 'CP2077.exe')
    assert.equal(result[0]['canonical-name'], 'Cyberpunk 2077')
    assert.deepEqual(result[0]['cover-search-terms'], ['Cyberpunk 2077'])

    assert.equal(result[1].name, 'weird.exe')
    assert.equal(result[1]['canonical-name'], 'Unknown Game')
    assert.equal(result[1]['ai-confidence'], 0.4)
    assert.equal(JSON.parse(requestBody.messages[1].content).locale, 'zh')
    assert.match(requestBody.messages[0].content, /Simplified Chinese/)

    const cachedResult = await enhanceScannedGameNames([
      { name: 'CP2077.exe', cmd: 'C:/Games/Cyberpunk2077/bin/x64/Cyberpunk2077.exe' },
      { name: 'weird.exe', cmd: 'C:/Games/weird.exe' },
    ])

    assert.equal(cachedResult[0].name, 'Cyberpunk 2077')
    assert.equal(cachedResult[1]['canonical-name'], 'Unknown Game')
    assert.equal(fetchCalls, 1)
  } finally {
    globalThis.fetch = originalFetch
    if (originalDocument === undefined) {
      delete globalThis.document
    } else {
      globalThis.document = originalDocument
    }
  }
})

test('enhanceScannedGameNames skips user override entries', async () => {
  const originalFetch = globalThis.fetch
  let fetchCalls = 0
  globalThis.fetch = async () => {
    fetchCalls += 1
    throw new Error('AI should not be called for user overrides')
  }

  try {
    const result = await enhanceScannedGameNames([
      {
        name: 'Confirmed Game',
        cmd: 'C:/Games/Confirmed/game.exe',
        'user-override': true,
        'ai-confidence': 1,
      },
    ])

    assert.equal(result[0].name, 'Confirmed Game')
    assert.equal(fetchCalls, 0)
  } finally {
    globalThis.fetch = originalFetch
  }
})

test('enhanceScannedGameNames skips failed batches and keeps later successes', async () => {
  __aiCacheTestUtils.clearMemoryStores()
  const originalFetch = globalThis.fetch
  const originalWarn = console.warn
  let fetchCalls = 0

  console.warn = () => {}
  globalThis.fetch = async () => {
    fetchCalls += 1
    if (fetchCalls === 1) {
      throw new Error('temporary metadata outage')
    }

    return {
      ok: true,
      async json() {
        return {
          choices: [
            {
              message: {
                content: JSON.stringify({
                  items: [
                    {
                      id: '20',
                      displayName: 'Recovered Game',
                      canonicalName: 'Recovered Game',
                      searchTerms: ['Recovered Game'],
                      isGame: true,
                      confidence: 0.95,
                    },
                  ],
                }),
              },
            },
          ],
        }
      },
    }
  }

  try {
    const apps = Array.from({ length: 21 }, (_, index) => ({ name: `game-${index}.exe` }))
    const result = await enhanceScannedGameNames(apps)

    assert.equal(fetchCalls, 2)
    assert.equal(result[0].name, 'game-0.exe')
    assert.equal(result[20].name, 'Recovered Game')
    assert.equal(result[20]['canonical-name'], 'Recovered Game')
  } finally {
    globalThis.fetch = originalFetch
    console.warn = originalWarn
  }
})
