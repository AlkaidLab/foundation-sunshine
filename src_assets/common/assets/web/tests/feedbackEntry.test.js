import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// 反馈: a tile at the top of 帮助与关于 (the About page) and an action of the search; the form is MErrorHost of the shell
const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('the About page has the 反馈问题 tile with the others (体检 starts from its tile, the form from the tile of 反馈) and the search runs 反馈', () => {
  const about = read('../views/About.vue')
  assert.match(about, /<MHelpTiles[^]*@doctor="shell\?\.nav\.go\('doctor', \{ autostart: true \}\)"[^]*\/>/)
  assert.ok(!/<MFeedbackCard/.test(about), 'the tile opens the form: no second entry in the page')
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /feedback: \(\) => \{ useErrors\(\)\.reportFeedback\(\) \}/)
  assert.match(shell, /<MErrorHost v-if="!embedded"/, 'an embedded frame has no form host; the row hides itself there')
})

test('the Web UI tells the feedback sheet that its host takes a screenshot, and has no capture of its own (a browser cannot take one silently)', () => {
  const shell = read('../views/WebShell.vue')
  const reg = shell.slice(shell.indexOf('registerDevReport({'), shell.indexOf('let removeErrorHandlers'))
  assert.match(reg, /shots: true/)
  assert.match(reg, /product: 'web'/)
  assert.doesNotMatch(reg, /capture:/, 'no silent capture of the page: the person chooses, pastes or drops a picture')
})
