import assert from 'node:assert/strict'
import test from 'node:test'

import { extractReleaseDetails } from '../utils/releaseDetails.js'

test('release details extract a changelog before contributors', () => {
  const body = [
    '<h2>What&#39;s Changed</h2>',
    '<p>Fixed the updater.</p>',
    '<p><strong>Full Changelog</strong>: <a href="https://example.com/compare">compare</a></p>',
    '<hr>',
    '<h2>Contributors</h2>',
    '<p><a href="https://example.com/user">@user</a></p>',
  ].join('\n')

  assert.deepEqual(extractReleaseDetails(body), {
    notes: '<p>Fixed the updater.</p>',
    fullChangelog: 'https://example.com/compare',
    contributors: '<p><a href="https://example.com/user">@user</a></p>',
  })
})

test('release details extract a changelog after new contributors', () => {
  const body = [
    "<h2>What's Changed</h2>",
    '<p>Improved streaming.</p>',
    '<h2>New Contributors</h2>',
    '<ul><li><a href="https://example.com/new-user">@new-user</a></li></ul>',
    '<p><strong>Full Changelog:</strong> <a href="https://example.com/compare-next">compare</a></p>',
    '<hr />',
  ].join('\n')

  assert.deepEqual(extractReleaseDetails(body), {
    notes: '<p>Improved streaming.</p>',
    fullChangelog: 'https://example.com/compare-next',
    contributors: '<ul><li><a href="https://example.com/new-user">@new-user</a></li></ul>',
  })
})

test('release details preserve an unrelated leading heading', () => {
  const body = '<h2>Highlights</h2>\n<p>Lower latency.</p>'

  assert.deepEqual(extractReleaseDetails(body), {
    notes: body,
    fullChangelog: '',
    contributors: '',
  })
})
