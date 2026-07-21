import test from 'node:test'
import assert from 'node:assert/strict'
import {
  SETUP_WIZARD_LANGUAGE_SAVED_KEY,
  useSetupWizard,
} from '../composables/useSetupWizard.js'

function setNavigatorLanguages(languages) {
  Object.defineProperty(globalThis, 'navigator', {
    value: {
      languages,
      language: languages[0],
    },
    configurable: true,
  })
}

function setSessionStorage(initial = {}) {
  const values = new Map(Object.entries(initial))
  Object.defineProperty(globalThis, 'sessionStorage', {
    value: {
      getItem: (key) => values.get(key) ?? null,
      setItem: (key, value) => values.set(key, String(value)),
      removeItem: (key) => values.delete(key),
    },
    configurable: true,
  })
  return values
}

test('setup wizard shows language step for default English on Chinese browsers', () => {
  setNavigatorLanguages(['zh-CN'])
  setSessionStorage()
  const wizard = useSetupWizard()

  assert.equal(wizard.checkSetupWizard({
    setup_wizard_completed: false,
    locale: 'en',
  }), true)

  assert.equal(wizard.hasLocale.value, false)
})

test('setup wizard continues after language save reload in Chinese browsers', () => {
  setNavigatorLanguages(['zh-CN'])
  setSessionStorage({ [SETUP_WIZARD_LANGUAGE_SAVED_KEY]: 'true' })
  const wizard = useSetupWizard()

  assert.equal(wizard.checkSetupWizard({
    setup_wizard_completed: false,
    locale: 'en',
  }), true)

  assert.equal(wizard.hasLocale.value, true)
})

test('setup wizard skips language step for non-English saved locales', () => {
  setNavigatorLanguages(['zh-CN'])
  setSessionStorage()
  const wizard = useSetupWizard()

  assert.equal(wizard.checkSetupWizard({
    setup_wizard_completed: false,
    locale: 'zh',
  }), true)

  assert.equal(wizard.hasLocale.value, true)
})

test('setup wizard keeps default English skip behavior outside Chinese browsers', () => {
  setNavigatorLanguages(['en-US'])
  setSessionStorage()
  const wizard = useSetupWizard()

  assert.equal(wizard.checkSetupWizard({
    setup_wizard_completed: false,
    locale: 'en',
  }), true)

  assert.equal(wizard.hasLocale.value, true)
})

test('setup wizard clears language reload marker after completion', () => {
  setNavigatorLanguages(['zh-CN'])
  const storage = setSessionStorage({ [SETUP_WIZARD_LANGUAGE_SAVED_KEY]: 'true' })
  const wizard = useSetupWizard()

  assert.equal(wizard.checkSetupWizard({
    setup_wizard_completed: true,
    locale: 'en',
  }), false)

  assert.equal(storage.has(SETUP_WIZARD_LANGUAGE_SAVED_KEY), false)
})
