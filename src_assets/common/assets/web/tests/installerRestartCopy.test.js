import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

// Restart in the installer: most installs need none, so the welcome page says nothing
// about it and there is no restart page. The done page offers an optional, weak "Restart PC" button next to
// the primary next step; nothing on it explains a restart. Detection and the RunOnce resume stay as they are.
const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..', '..')
const installerUi = readFileSync(join(root, 'tools/installer_ui.h'), 'utf8')
const installerShell = readFileSync(join(root, 'tools/installer_shell_ui.inc'), 'utf8')
const installerState = readFileSync(join(root, 'tools/installer_d2d.h'), 'utf8')
const installerScript = readFileSync(join(root, 'cmake/packaging/sunshine.iss.in'), 'utf8')

const doneStart = installerUi.indexOf('case pg::done:')
const doneCase = installerUi.slice(doneStart, installerUi.indexOf('if (plain)', doneStart))

test('the welcome page has two facts and none about a restart', () => {
  const welcome = installerUi.slice(installerUi.indexOf('case pg::welcome:'), installerUi.indexOf('case pg::maintenance:'))
  assert.match(welcome, /L"管理员权限", L"Administrator permission"/)
  assert.match(welcome, /L"组件", L"Components"/)
  assert.doesNotMatch(welcome, /重启|Restart/)
  assert.equal((welcome.match(/\{ ic::/g) || []).length, 2)
})

test('there is no restart page, and no "restart later" button', () => {
  assert.doesNotMatch(installerState, /enum class pg \{[^}]*\brestart\b/)
  assert.doesNotMatch(installerUi + installerShell, /pg::restart|restart_later|稍后重启|Restart later/)
})

test('the done page: the next step is primary, "Restart PC" is a weak text button, no restart text', () => {
  assert.match(doneCase, /add_button\(101,[^;]*开启串流旅程[^;]*bkind::filled/)
  assert.match(doneCase, /add_button\(102, Z\(v, L"重启电脑", L"Restart PC"\), bkind::text\)/)
  assert.doesNotMatch(doneCase, /重启后生效|take effect after a restart/)
  assert.match(installerShell, /else \{\s*restart_windows\(\);/)
})

test('Escape on the done page closes the window and never restarts the PC', () => {
  assert.match(installerShell, /case pg::done:\s*DestroyWindow\(S\.window\);/)
})

test('a needs-reboot result shows the done page; the virtual display alone does not set RunOnce', () => {
  const block = installerShell.slice(installerShell.indexOf('state == "needs-reboot"'), installerShell.indexOf('state == "component-failed"'))
  assert.match(block, /reboot_reasons != "vdd"\) write_restart_resume\(\)/)
  assert.match(block, /go\(pg::done\)/)
})

test('no installer sentence claims the virtual gamepad needs a restart', () => {
  const claims = [
    /只有驱动明确要求时/,
    /Windows is asked to restart only when a driver says it must/,
    /虚拟手柄[^。\n]{0,13}(需要|必须|得)[^。\n]{0,6}重启/,
    /(virtual )?gamepad (needs|requires|must)(?! no)[^.]{0,20}(restart|reboot)/i,
  ]
  for (const [name, text] of [['installer_ui.h', installerUi], ['sunshine.iss.in', installerScript]]) {
    for (const pattern of claims) {
      const hit = text.match(pattern)
      assert.equal(hit, null, `${name}: ${hit && hit[0]}`)
    }
  }
})
