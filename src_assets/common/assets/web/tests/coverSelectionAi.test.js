import test from 'node:test'
import assert from 'node:assert/strict'

import { pickFallbackCoverCandidate } from '../utils/coverSelectionAi.js'

test('pickFallbackCoverCandidate prefers exact title matches', () => {
  const app = {
    name: 'Elden Ring',
    'canonical-name': 'Elden Ring',
    'cover-search-terms': ['Elden Ring', '艾尔登法环'],
  }

  const selected = pickFallbackCoverCandidate(app, [
    { name: 'Elden Ring: Nightreign', source: 'steam', saveUrl: 'nightreign.jpg' },
    { name: 'Elden Ring', source: 'igdb', saveUrl: 'elden-ring.jpg' },
  ])

  assert.equal(selected.saveUrl, 'elden-ring.jpg')
})
