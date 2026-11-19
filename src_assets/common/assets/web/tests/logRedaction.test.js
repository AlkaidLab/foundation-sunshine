import assert from 'node:assert/strict'
import fs from 'node:fs'
import test from 'node:test'

const read = (name) => fs.readFileSync(new URL(name, import.meta.url), 'utf8')

test('diagnosis sends the shared redacted body and does not fetch logs itself', () => {
  const diagnosis = read('../composables/useAiDiagnosis.js')
  assert.match(diagnosis, /diagnosisRequestBody/)
  assert.match(diagnosis, /openLogDiagnosis/)
  assert.doesNotMatch(diagnosis, /\/api\/logs/)
})

test('game title and cover requests are not host logs', () => {
  for (const name of ['../utils/gameMetadataAi.js', '../utils/coverSelectionAi.js']) {
    assert.doesNotMatch(read(name), /\/api\/logs/, name)
  }
})
