import test from 'node:test'
import assert from 'node:assert/strict'

import { getPromptLanguageName } from '../utils/aiLocale.js'

test('getPromptLanguageName maps supported Sunshine locales', () => {
  assert.equal(getPromptLanguageName('zh'), 'Simplified Chinese')
  assert.equal(getPromptLanguageName('zh-TW'), 'Traditional Chinese')
  assert.equal(getPromptLanguageName('pt-BR'), 'Brazilian Portuguese')
  assert.equal(getPromptLanguageName('de'), 'German')
  assert.equal(getPromptLanguageName('unknown'), 'English')
})
