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
const installerWeb = readFileSync(join(root, 'tools/installer_web.inc'), 'utf8')
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

test('the virtual mouse installer is never offered or run on Windows 10', () => {
  assert.match(installerScript, /Name: "vmouse";[^\n]*MinVersion: 10\.0\.22000/)
  assert.match(installerScript, /Lowercase\(Name\) = 'vmouse'[\s\S]*?Result := False;/)
  assert.match(installerShell, /tile == 3 \? 22000/)
  assert.match(installerWeb, /component_min_build\(i\) <= windows_build\(\)\) all = all && v\.tiles\[i\]/)
  assert.match(installerShell, /component_min_build\(i\) <= windows_build\(\) && !all/)
  assert.match(installerShell, /begin_install\(\) \{[\s\S]*?apply_os_limits\(S\.v\);[\s\S]*?g_run\.components = component_list\(\)/)
})

test('the installer closes itself a moment after the Panel was started, and keeps the window when the Panel did not start', () => {
  const click = installerShell.slice(installerShell.indexOf('S.v.panel_failed = !launch_panel();'), installerShell.indexOf('restart_windows();  // the weak button'))
  assert.match(click, /if \(!S\.v\.panel_failed\) SetTimer\(S\.window, kCloseTimer, kCloseAfterLaunchMs, nullptr\);/)
  const timer = installerShell.slice(installerShell.indexOf('case WM_TIMER:'), installerShell.indexOf('case WM_DPICHANGED'))
  assert.match(timer, /wparam == kCloseTimer[\s\S]*KillTimer\(window, kCloseTimer\);[\s\S]*S\.v\.page == pg::done && !S\.v\.panel_failed\) DestroyWindow\(window\)/)
  assert.match(installerShell, /case WM_DESTROY:\s*KillTimer\(window, kTimer\);\s*KillTimer\(window, kCloseTimer\);/)
  assert.match(readFileSync(join(root, 'tools/installer_shell.cpp'), 'utf8'), /constexpr UINT_PTR kCloseTimer = 8;/)
  // a Panel that did not start shows the retry on the same button
  assert.match(doneCase, /管理界面未能打开。点“开启串流旅程”重试。/)
})

test('the maintenance page names the action on its primary button and leaves the version to the lead', () => {
  const maintenance = installerUi.slice(installerUi.indexOf('case pg::maintenance:'), installerUi.indexOf('case pg::components:'))
  assert.match(maintenance, /add_button\(101, action, bkind::filled\)/)
  assert.doesNotMatch(maintenance, /add_button\(101, action \+/, 'a long version string made the button wide enough to run into its neighbours')
  assert.match(maintenance, /L"降级", L"Downgrade"/)
  assert.match(maintenance, /L"重新安装", L"Reinstall"/)
  assert.match(maintenance, /L"更新", L"Update"/)
  assert.match(maintenance, /v\.package_version/, 'the package version is still shown in the lead')
})

test('the buttons on the right never run into the text button on the left of the maintenance page', () => {
  const layout = installerUi.slice(installerUi.indexOf('A text button on the left (卸载)'), installerUi.indexOf('out.buttons_top = by;'))
  assert.match(layout, /left_end \+ 12/, 'a gap of 12 px')
  assert.match(layout, /c\.measure_w\(b->text, 15, 1\) \+ 24/, 'a button gives up padding, never its text')
  assert.match(layout, /b->x = at;/, 'the right buttons are laid out again after they shrank')
})
