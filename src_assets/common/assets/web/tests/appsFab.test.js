import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('the 应用 page puts its buttons in the dock at the content pane (MSceneFab); the list pane and the shell no longer carry a button', () => {
  const apps = read('../views/Apps.vue')
  const shell = read('../views/WebShell.vue')
  assert.ok(/<MSceneFab v-if="isLoaded">[\s\S]*apps\.add_new[\s\S]*<\/MSceneFab>/.test(apps))
  assert.ok(!/class="apps-fabs"/.test(apps) && !/noListPane/.test(apps))
  // where the host can scan, one FAB menu (添加 ▸ 手动添加, 扫描游戏库) in place of two stacked FABs
  // (the items and the choice are in the script: the Sparxie component adds two entries to the same menu while it is installed)
  assert.match(apps, /<MFabMenu v-if="canScan\(\) \|\| spxAvailable" icon="add"[\s\S]*?:items="fabItems" @select="onFab"/)
  assert.match(apps, /id: 'scan'[\s\S]*?item\.id === 'scan'\) openScanOptions\(\)[\s\S]*?else newApp\(\)/)
  assert.ok(!/apps-fab--scan/.test(apps))
  assert.ok(!/list-fab|ShellFab/.test(shell))
  assert.ok(!fs.existsSync(new URL('../components/layout/ShellFab.vue', import.meta.url)))
})

test('the add action of a list row and of the home page still reaches the page (pending request)', () => {
  const apps = read('../views/Apps.vue')
  assert.ok(/webShell\?\.pending\.value[\s\S]*request !== 'addApp'/.test(apps))
})
