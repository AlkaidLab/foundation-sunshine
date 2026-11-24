import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('the connect dialog of the Web UI says why a PIN failed', () => {
  const shell = read('../views/WebShell.vue')
  assert.match(shell, /path === '\/api\/pin' && !r\.ok\) throw failing\(r\)/)
  for (const key of ['pinIncorrect', 'pinTimeout', 'pinNetwork', 'pinNotReady']) assert.match(shell, new RegExp(`T\\.${key}`))
})
