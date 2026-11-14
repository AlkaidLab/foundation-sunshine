import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// 输入与外设 is four pages in the Web UI: /config#tab=input&sub=gamepad|keyboard|mouse|usb, a list like 音频; there is no 高级 section and no drivers page
const read = (p) => readFile(new URL(p, import.meta.url), 'utf8')

test('the input tab is four places and nothing else: 手柄, 键盘, 鼠标与触控, USB 转发', async () => {
  const inputs = await read('../configs/tabs/Inputs.vue')
  assert.deepEqual([...inputs.matchAll(/<CfgSub (?:v-if="[^"]*" )?id="(\w[\w-]*)"/g)].map((m) => m[1]), ['gamepad', 'keyboard', 'mouse', 'usb'])
  assert.ok(!/<CfgSub[^>]*id="(expert|drivers)"/.test(inputs) && !/<CfgTop/.test(inputs), 'no 高级 page, no drivers page: the items are at the bottom of their own page')
  assert.match(inputs, /<CfgSub v-if="platform === 'windows'" id="usb"/, 'USB forwarding is a Windows host setting')
  const keyboard = inputs.slice(inputs.indexOf('<CfgSub id="keyboard"'), inputs.indexOf('<CfgSub id="mouse"'))
  for (const key of ['keyboard', 'always_send_scancodes', 'key_rightalt_to_key_win', 'key_repeat_delay', 'key_repeat_frequency']) assert.ok(keyboard.includes(`k="${key}"`), key)
})

test('the shell has the four pages as a list of the rail, the way audio has', async () => {
  const shell = await read('../views/WebShell.vue')
  for (const id of ['gamepad', 'keyboard', 'mouse', 'usb']) {
    assert.match(shell, new RegExp(`${id}: \\{ tab: 'input', sub: '${id}' \\}`), `${id} is a place`)
    assert.match(shell, new RegExp(`\\n  ${id}: \\{ rail: 'input', place: true,`), `${id} is a page of the rail`)
  }
  assert.ok(!/gamepad: \{ rail: 'input', place: true, list: false/.test(shell), 'the list of 输入与外设 is drawn')
  assert.match(shell, /usb: \{ rail: 'input', place: true, windows: true/)
})

test('a page inside the gamepad page opens the gamepad place; Back of such a page goes to the place', async () => {
  const shell = await read('../views/WebShell.vue'); const config = await read('../views/Config.vue')
  assert.match(shell, /\(params\?\.tab === 'input' && params\.sub \? 'gamepad' : undefined\)/)
  assert.match(config, /const isPlace = computed\(\(\) => !!PLACE_TABS\[tab\.value\]\)/)
  assert.match(config, /parentPlace\(tab\.value, subNav\.current\.value\) === subNav\.current\.value/)
  assert.match(config, /subNav\.open\(DEFAULT_PLACE\[id\], true\)/, 'a tab of places never opens empty, and the entry it opens in is the one the shell pushed')
})

test('the settings entry of the narrow window lists every page of the five categories in sections: any page is two presses away', async () => {
  const shell = await read('../views/WebShell.vue')
  const hub = shell.slice(shell.indexOf("VIEWS['settings-hub']"), shell.indexOf('// Pages that show a supporting pane'))
  assert.match(hub, /h\(MSection, \{ key: id, title: t\(RAIL\.find/)
  assert.match(hub, /hubPages\(id\)\.map/)
  assert.ok(!/nav\.selectRail\(id\)/.test(hub), 'a row of the entry opens a page, not another list')
})

test('the list of a rail has its title at the top left where the list is a page of its own (the bottom bar of a narrow window), as 设置 has', async () => {
  const shell = await read('../views/WebShell.vue')
  assert.ok(shell.includes('<template #list="{ entry, scene }">'))
  assert.ok(shell.includes(':title="railLabel(entry.rail)"') && shell.includes(':titled="scene.kind === \'single\'"'))
})

test('the rail that holds 常规, 配置文件, 网络 and 安全与登录 is named for the host, in both languages', async () => {
  const zh = JSON.parse(await read('../public/assets/locale/zh.json')); const en = JSON.parse(await read('../public/assets/locale/en.json'))
  assert.equal(zh.shell.stream_settings, '主机设置'); assert.equal(en.shell.stream_settings, 'Host settings')
  assert.match(await read('../views/WebShell.vue'), /id: 'config', icon: 'tune', label: 'shell\.stream_settings'/)
})

test('the page of the logs is 诊断与日志 in the shell and on the page, and the old address still opens it', async () => {
  const zh = JSON.parse(await read('../public/assets/locale/zh.json')); const en = JSON.parse(await read('../public/assets/locale/en.json'))
  assert.equal(zh.shell.logs_diagnostics, '诊断与日志'); assert.equal(zh.troubleshooting.troubleshooting, '诊断与日志'); assert.equal(zh.navbar.troubleshoot, '诊断与日志')
  assert.equal(en.shell.logs_diagnostics, 'Diagnostics and logs'); assert.equal(en.troubleshooting.troubleshooting, 'Diagnostics and logs'); assert.equal(en.navbar.troubleshoot, 'Diagnostics and logs')
  const shell = await read('../views/WebShell.vue')
  assert.match(shell, /troubleshooting: \{ rail: 'system', path: '\/troubleshooting'/)
  assert.match(shell, /if \(p === '\/diagnostics'\) return 'troubleshooting'/)
})
