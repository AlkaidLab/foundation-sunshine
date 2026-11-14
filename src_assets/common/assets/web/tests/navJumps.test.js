import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// The moves the product makes for the person (a search action, a status notice, the connect dialog, the doctor entry) go through the jump history, so the rail and the list follow and the chip at the top of the page names the way back.
// The shell needs the bundler: its wiring is read from the source.
const read = (p) => readFile(new URL(p, import.meta.url), 'utf8')

test('search actions, status notices, the component page and the connect dialog move through the jump history', async () => {
  const shell = await read('../views/WebShell.vue')
  for (const line of [
    /@open-downloads="\(\) => \{ connect\.open\.value = false; jumpNav\.go\('downloads'\) \}"/,
    /openPage: \(\) => jumpNav\.go\('components'\), openAbout: \(\) => jumpNav\.go\('about'\)/,
    /go: \{ components: \(\) => jumpNav\.go\('components'\), microphone: \(\) => jumpNav\.go\('microphone'\), vdd: \(\) => jumpNav\.go\('vdd'\), update: \(\) => jumpNav\.go\('about'\) \}/,
    /logs: \(\) => jumpNav\.go\('troubleshooting'\),\s*\n\s*exportDiag: \(\) => jumpNav\.go\('troubleshooting'\)/,
    /doctor: \(\) => jumpNav\.go\('doctor', \{ autostart: true \}\)/,
  ]) assert.match(shell, line)
})

test('the person\'s own moves stay on the plain stack: the rail, a row of the list, the page\'s own back', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /const onRail = \(item\) => \(item\.id === 'settings-hub' \? nav\.go\('settings-hub'\) : nav\.selectRail\(item\.id\)\)/)
  assert.match(shell, /const onNavigate = \(item\) => \(item\.route === 'config' \? openConfig\(item\.params\) : nav\.go\(item\.route \|\| item\.id, item\.params\)\)/)
  assert.match(shell, /const jumpNav = \{ \.\.\.nav, go: \(id, params, opts\) => jumps\.jump\(\(\) => nav\.go\(id, params, opts\)\) \}/)
})
