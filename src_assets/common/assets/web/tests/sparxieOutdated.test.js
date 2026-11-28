import test from 'node:test'
import assert from 'node:assert/strict'
import { normalizeStatus } from '../services/sparxieClient.js'
import { copyFor } from '../utils/sparxie/copy.js'
import { usesSparxie, withDefaults } from '../utils/sparxie/model.js'
import { resetOutdatedWarnings, warnOutdated } from '../utils/sparxie/outdated.js'

test('an outdated Sparxie is said in a dialog that names it and the release to use: once for each on the pages, every time at an import', async () => {
  resetOutdatedWarnings()
  const shown = []
  const alert = async (message, title, options) => { shown.push([message, title, options.confirmText]) }
  const status = normalizeStatus({ installed: true, outdatedVersion: '0.2.0', latestVersion: '0.2.1' })
  assert.equal(await warnOutdated(status, copyFor('zh'), { alert }), true)
  assert.deepEqual(shown[0], ['Sparxie 0.2.0 已过时，请换用 0.2.1 或更新版本', 'Sparxie 版本已过时', '知道了'])
  assert.equal(await warnOutdated(status, copyFor('zh'), { alert }), false)
  assert.equal(await warnOutdated(status, copyFor('en'), { alert, always: true }), true)
  assert.equal(shown[1][0], 'Sparxie 0.2.0 is outdated. Use 0.2.1 or newer')
  // a current release, and a host that says nothing about it, warn of nothing
  assert.equal(await warnOutdated(normalizeStatus({ installed: true, outdatedVersion: '' }), copyFor('zh'), { alert, always: true }), false)
  assert.equal(await warnOutdated(normalizeStatus({ installed: true }), copyFor('zh'), { alert, always: true }), false)
  assert.equal(shown.length, 2)
})

test('keyboard and mouse without the unlock asks nothing of Sparxie: the page sends a direct start, as the host decides', () => {
  for (const game of ['genshin', 'starrail', 'endfield']) {
    assert.equal(usesSparxie(game, { input: 'kbm', fpsUnlock: false }), false, game)
    assert.equal(usesSparxie(game, { input: 'kbm', fpsUnlock: true }), true, game)
    assert.equal(usesSparxie(game, { input: 'touch', fpsUnlock: false }), true, game)
  }
  assert.equal(usesSparxie('zzz', { input: 'kbm', fpsUnlock: true }), false, 'Zenless Zone Zero has no unlock')
  assert.equal(usesSparxie('zzz', { input: 'touch' }), true)
})

test('editing a direct application that never chose the unlock keeps it off: saving does not send it through Sparxie', () => {
  assert.equal(withDefaults({ mode: 'pc', game: 'genshin', options: {} }).options.fpsUnlock, false)
  assert.equal(withDefaults({ mode: 'pc', game: 'genshin', options: { fpsUnlock: true } }).options.fpsUnlock, true)
  assert.equal(withDefaults({ mode: 'touch', game: 'genshin', options: {} }).options.fpsUnlock, true)
})
