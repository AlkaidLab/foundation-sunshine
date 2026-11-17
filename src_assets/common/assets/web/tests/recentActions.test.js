import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// 最近操作 and the live controls of a running stream on the overview of the Web UI
const read = (p) => readFile(new URL(p, import.meta.url), 'utf8')

test('the overview gives the tile the keys used last, whether the host takes its commands from this browser, and the DualSense snapshot', async () => {
  const home = await read('../views/Home.vue')
  assert.match(home, /recent: recentKeys\.value, canRuntime: live\.local\.value !== false, ds5Keys: shellLive\.dualsenseKeys\.value/)
  assert.match(home, /case 'runtime-bitrate': runtimeBitrate\(a\.value\); break/); assert.match(home, /case 'ds5-tune': ds5Tune\(a\.value\); break/)
  assert.match(home, /\/api\/runtime\/bitrate\?bitrate=\$\{kbps\}&clientname=\$\{encodeURIComponent\(client\)\}/)
  assert.match(home, /apiPostJson\('\/api\/dualsense\/tuning', \{ schema: 'ALK-DS5T\/1', values: \{ \[key\]: value \} \}\)/)
})

test('a setting saved from a tile is a recent one (the configuration page remembers its own changes: Config.vue noteChanges)', async () => {
  const home = await read('../views/Home.vue'); const config = await read('../views/Config.vue')
  assert.match(home, /hostConfig\.value = r\.config; rememberRecent\(key\)/)
  assert.match(config, /settingsList\.remember\(key\)/)
})

// the window's own context menu (SPEC-context-menu) in the Web UI
test('the shell mounts the context menu with what the Web UI can do; the configuration page adds the setting row\'s items', async () => {
  const shell = await read('../views/WebShell.vue'); const config = await read('../views/Config.vue')
  assert.match(shell, /<MContextProvider :locale="String\(uiLocale\)" :callbacks="ctxCallbacks" :capabilities="\{ product: 'web', print: true, developer: developerMode\.enabled\.value \}" \/>/)
  assert.match(shell, /const ctxCallbacks = \{ search: \(text\) => \{ uni\.query\.value = text; uni\.show\(\) \}, connect: \(\) => \{ connect\.open\.value = true \}/)
  assert.match(config, /registerContext\('setting', \(\{ id, el, T, cb \}\) =>/)
  assert.match(config, /run: \(\) => \{ delete config\.value\[id\] \}/); assert.match(config, /copySettingLink/); assert.match(config, /offSettingMenu\(\)/)
})

// the object actions: each runs the function of the control it mirrors
test('the object actions: 运行体检 is 全部检查, 这是什么 shows the row\'s info tip, 固定 is the pin host\'s toggle, a client row copies its device id; the page inside the panel has the menu too', async () => {
  const shell = await read('../views/WebShell.vue'); const config = await read('../views/Config.vue'); const pin = await read('../views/Pin.vue')
  assert.match(shell, /doctor: \(\) => \{ componentStatus\.openPage\(\); void componentStatus\.checkAll\(\) \}/)
  assert.match(shell, /<MPinHost ref="pinHost" storage-key="web-home" :locale="String\(uiLocale\)" :forward="forwardPin">/)
  assert.match(shell, /<MContextProvider :locale="String\(uiLocale\)" :callbacks="embeddedCtxCallbacks" :capabilities="\{ product: 'web', developer: developerMode\.enabled\.value \}" \/>/)
  assert.match(shell, /else if \(entry\.id === 'pin'\) pinHost\.value\.toggle\(rowPinSpec\(item\)\)/); assert.match(shell, /const ROW_TILES = \{ pin: 'clients', apps: 'apps', downloads: 'downloads', troubleshooting: 'diagnostics' \}/)
  assert.match(config, /info\.focus\(\{ preventScroll: true \}\); showTip\(info\)/); assert.match(config, /run: \(\) => pinning\.toggle\(spec\)/)
  assert.match(pin, /id: 'copy-id'/); assert.match(pin, /writeText\(client\.uuid\)/)
})

test('the list of 串流 has no 添加应用 (it is an action of the page 串流应用, its FAB); the rail and the list are two presses to that page', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.ok(!/#top/.test(shell) && !/m-navlist__add/.test(shell) && !/c_act-add-app/.test(shell), 'no button in the list pane')
  const hub = shell.slice(shell.indexOf("VIEWS['stream-hub']"), shell.indexOf('// Pages that show a supporting pane'))
  assert.ok(!/actions:|MButton/.test(hub), 'the phone hub only lists pages')
  const apps = await read('../views/Apps.vue')
  assert.match(apps, /<MSceneFab v-if="isLoaded">[\s\S]*?newApp[\s\S]*?<\/MSceneFab>/, 'adding is on the page itself')
  assert.match(apps, /pending/); assert.match(shell, /addApp: \(\) => \{ nav\.go\('apps'\); pending\.value = 'addApp' \}/, 'search and the home still ask the page to add')
})
