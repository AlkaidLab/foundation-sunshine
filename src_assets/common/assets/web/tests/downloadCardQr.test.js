import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// The QR code of a download card sits at the bottom of the card, on the links, so it is at one height in every card of a row. A card of a partner client has no release tags to reserve room for
// (tagsReserve), so a code placed right under the tags came out 32 px higher than in the cards of the team's own clients.
const less = await readFile(new URL('../styles/m3e-web.less', import.meta.url), 'utf8')

test('the QR code of a download card is pushed to the bottom with the links right under it', () => {
  assert.match(less, /\.dl-card__qr \{ margin: auto 0 0;/)
  assert.match(less, /\.dl-card__qr ~ \.dl-card__links \{ margin-top: 0; \}/)
  assert.match(less, /\.dl-card__links \{[^}]*margin-top: auto;/, 'a card without a code still keeps its links at the bottom')
})
