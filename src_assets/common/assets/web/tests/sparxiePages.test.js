import assert from 'node:assert/strict'
import { existsSync, readFileSync } from 'node:fs'
import { COPY } from '../utils/sparxie/copy.js'
const COPYZH = COPY.zh
import test from 'node:test'

const read = (p) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8')
const copy = read('utils/sparxie/copy.js')
const apps = read('views/Apps.vue')
const editor = read('components/AppEditor.vue')
const game = read('components/sparxie/SparxieOptions.vue')
const rows = read('components/sparxie/SparxieFieldRows.vue')
const row = read('components/sparxie/SparxieFieldRow.vue')
const create = read('components/sparxie/SparxieNewApp.vue')
const settingsC = read('components/sparxie/SparxieSettings.vue')
const rowC = read('components/sparxie/SparxieInstallRow.vue')
const cover = read('components/sparxie/SparxieCover.vue')
const notice = read('components/sparxie/SparxieNotice.vue')
const tpl = (src) => src.slice(src.indexOf('<template>'), src.indexOf('<script setup>'))

test('添加游戏 is there for every host that has the game routes, with or without the component', () => {
  assert.ok(/if \(spxAvailable\.value\) items\.push\(\{ id: 'spx-new'/.test(apps))
  assert.ok(/<SparxieNewApp v-if="spxAvailable && spxPage === 'new'"/.test(apps))
  assert.ok(!/localStorage|spx-concept|isConcept/.test(apps + editor + game + create), 'no concept switch in the product pages')
})

test('the cards carry the chips under the title in one row (+N for the rest), and a followed poster comes from the art cache by version', () => {
  assert.ok(apps.includes('<MChipRow v-if="item.chips?.length" :chips="item.chips"'))
  assert.ok(/artUrl\('card'[^)]*block\.artVersion\)/.test(apps))
  assert.ok(!/class="apps-chips"/.test(apps), 'the old wrapping chip span is gone')
})

test('adding and editing a game are full pages: no modal, no sheet, no dialog on them', () => {
  for (const [name, src] of [['new', create], ['touch', game], ['notice', notice]]) {
    assert.ok(!/<MDialog|<MSheet|<dialog/.test(tpl(src)), `${name} has no modal`)
  }
  assert.ok(create.includes('<MPage'))
})

test('the game settings are one section on the page 添加游戏, for a new game app and for an existing one, after Sparxie\'s own settings', () => {
  const t = tpl(create)
  assert.ok(t.includes('<SparxieSettings') && !/C\.secDisplay|C\.secQuality/.test(t))
  const order = ['<SparxieOptions', '<SparxieSettings', 'C.secNameCover'].map((k) => t.indexOf(k))
  assert.ok(order.every((n) => n >= 0) && [...order].sort((a, b) => a - b).join() === order.join(), 'the order of the sections')
  assert.ok(['C.secProfile', 'C.value.secFps', 'C.value.secUi', 'C.value.secAdv', 'C.value.secSparxie'].every((k) => game.includes(k)), 'Sparxie\'s own options')
})

test('the game settings follow the game: no mode switch; an item the person changed is the application\'s own, marked changed with a way back, and 全部跟随游戏 returns them all; a running game locks nothing', () => {
  const t = tpl(settingsC)
  assert.ok(!t.includes('gsModeAuto') && !t.includes('gsModePreset') && !t.includes('settingsMode'), 'the two-mode switch is gone')
  assert.ok(!/gsModeAuto|gsModePreset|useCurrent/.test(copy), 'and so is its copy')
  assert.ok(t.includes('<GameField') && !t.includes(':disabled="running"') && t.includes(':locks="locks"'), 'the items are cells of the host\'s description; a running game does not lock them')
  assert.ok(t.includes('<MBanner v-if="running" tone="info" icon="lock">{{ C.gsRunning }}</MBanner>'), 'one line while the game runs')
  assert.ok(settingsC.includes('gameFieldsOf(props.row,'), 'the items are what the host describes for the title')
  assert.ok(settingsC.includes('client().gameSettings(props.installId') && settingsC.includes('setInterval(poll'), 'the game\'s own values are read always: they are what an item shows while it follows the game')
  assert.ok(!settingsC.includes('fillFromCurrent') && !settingsC.includes('fillWhenRead'), 'opening or reading never fills the application: nothing is held until the person changes an item')
  assert.ok(settingsC.includes('const followAll = ()') && t.includes('C.gsAllFollow') && t.includes('heldKeys.length'), '全部跟随游戏, there while something is held')
  assert.ok(settingsC.includes('block.restore') && t.includes('C.restoreNote'), 'restore or keep, said exactly')
  const field = read('components/sparxie/GameField.vue')
  assert.ok(field.includes('const held = computed(() => props.values[props.field.key] !== undefined)') && field.includes('const back = () => { delete props.values[props.field.key] }'), 'an item is held only after it was set; the way back removes it')
  assert.ok(!/followAll|gsChosen/.test(field), 'the per-item way back lives in the item')
  assert.equal(COPYZH.gsFollow, '跟随游戏'); assert.equal(COPYZH.gsChanged, '已更改'); assert.equal(COPYZH.gsAllFollow, '全部跟随游戏')
  assert.equal(COPY.en.gsFollow, 'Follows the game'); assert.equal(COPY.en.gsAllFollow, 'Follow the game for all')
  assert.ok(/游戏运行中自己改动过的项保留游戏的值/.test(COPYZH.restoreNote), 'the restore note says what happens to an item the game changed')
})

test('a game app is edited on the page 添加游戏 in its editing state; the application editor keeps every other application and offers the game way for a program it recognises', () => {
  assert.ok(/<AppEditor\s+v-if="editingApp && !isGameApp\(editingApp\)"/.test(apps) && /<SparxieNewApp v-if="editingApp && isGameApp\(editingApp\)" :edit="editingApp"/.test(apps))
  assert.ok(apps.includes('const isGameApp = (app) => spxAvailable.value && !!sparxieOf(app) && Number.isInteger(app.index) && app.index >= 0'))
  assert.ok(create.includes('const isEdit = !!props.edit') && create.includes('client().updateApp(props.edit.index, body)') && create.includes('emit(\'saved\', saved)'))
  assert.ok(create.includes('<SparxieOptions :block="optBlock" :fields="sparxieFields" />') && create.includes('<template v-if="reach(4) && touch">'), 'Sparxie\'s own settings are there for a new game app and an edited one')
  assert.ok(!editor.includes('SparxieGame') && editor.includes("emit('to-game', gameHint.path)"))
})

test('添加应用 recognises a program of a game: a banner with one tap to the game way, never a silent conversion', () => {
  assert.ok(tpl(editor).includes('<MBanner v-if="gameHint"') && tpl(editor).includes("@click=\"emit('to-game', gameHint.path)\""))
  assert.ok(editor.includes('spx.client().identify(exe)') && editor.includes('!isNewApp.value'), 'the host names the game and the region; only a new application is offered it')
  assert.ok(apps.includes('const takeOverToGame = (path) => { closeAppEditor(); spxPrefill.value = path; spxPage.value = \'new\' }') && apps.includes(':prefill="spxPrefill"'))
  assert.ok(create.includes('async function startPrefill()') && create.includes('pendingPrefill.value = { ...found.install'))
})

test('the + menu has three entries: 扫描游戏库, 添加游戏, 添加应用; Sparxie\'s profiles are a source of the scan, not an entry', () => {
  const items = apps.slice(apps.indexOf('const fabItems = computed'), apps.indexOf('const pageOpen'))
  assert.ok(items.indexOf("id: 'scan'") < items.indexOf("id: 'spx-new'") && items.indexOf("id: 'spx-new'") < items.indexOf("id: 'add'"))
  assert.ok(!items.includes('spx-import') && !apps.includes('SparxieImport'))
  assert.ok(apps.includes("'app-type': 'sparxie'") && apps.includes('sparxieClient().importProfiles('))
})

test('the scan list: one group per source with its own select-all, what is already in the list is marked 已添加 and cannot be picked, adapted games are marked 游戏应用', () => {
  const scan = read('components/ScanResultModal.vue')
  assert.ok(scan.includes('v-for="group in groups"') && scan.includes('toggleGroup(group)') && scan.includes("const SOURCES = ['steam', 'epic', 'gog', 'xbox', 'ea', 'ubisoft', 'battlenet', 'riot', 'hoyoplay', 'wegame', 'sparxie']"))
  assert.ok(scan.includes('spx.scanAdded') && scan.includes(':disabled="isAdded(app)"') && scan.includes('spx.scanGameApp'))
  assert.ok(apps.includes('const isScanAdded = ') && apps.includes('const gameOfScan = ') && apps.includes(':is-added="isScanAdded"') && apps.includes(':game-of="gameOfScan"'))
  assert.ok(apps.includes("await sparxieClient().createApp({ install: '', path: a.exe, region: g.region, mode: touchOk ? 'touch' : 'pc', name: a.name })"), 'an adapted game becomes a game app, with 自动 settings (none sent)')
  assert.ok(apps.includes('if (rest.length) await addAllScannedApps(rest)'), 'every other game is an ordinary application')
})

test('the statement about injection comes with Sparxie, as plain text: on the page until the first such app was created, and under its settings', () => {
  assert.ok(create.includes('<SparxieNotice v-if="touch && !status.warned"'))
  assert.ok(create.includes('<MFooter>{{ C.noticeFoot }}</MFooter>'))
  assert.ok(notice.includes('C.notice'))
  assert.ok(create.includes('acknowledgeNotice'), 'creating records that it was shown')
})

test('a closed-Sparxie requirement is stated on the create page with a way to check again', () => {
  assert.ok(create.includes("block === 'running'") && create.includes('C.closeSparxie') && create.includes('recheck'))
  assert.ok(create.includes('C.value.scanNone') && create.includes('C.value.wrongGame') && create.includes('C.value.mismatchRegion') && create.includes('C.value.scanTimeout'))
})

test('every place that marks Sparxie uses the mark picture: the chips, the group headers and the add-menu entries', () => {
  const model = read('utils/sparxie/model.js')
  assert.ok(model.includes('SPARXIE_MARK = SPARXIE_MARK_DATA') && model.includes('image: SPARXIE_MARK'))
  assert.equal((game.match(/<MChip tone="tertiary" :image="SPARXIE_MARK">Sparxie<\/MChip>/g) || []).length, 1, 'the header of each group of Sparxie\'s settings (one template, one per group)')
  assert.ok(create.includes(':image="SPARXIE_MARK"'))
  assert.ok(!/id: 'spx-new'[^}]*image:/.test(apps), '添加游戏 is every game\'s, not Sparxie\'s')
  assert.ok(apps.includes('appChips(app, spxCopy.value)'), 'the cards carry the chips, the Sparxie mark on touch apps')
})

test('the mark is a shipped product asset with its provenance', () => {
  assert.ok(existsSync(new URL('../public/images/sparxie-mark-64.png', import.meta.url)))
  // the host serves a short list of fixed /images files and this is not one of them: the pictures are inlined, and the inlined copy is the shipped PNG
  const png = readFileSync(new URL('../public/images/sparxie-mark-64.png', import.meta.url)).toString('base64')
  assert.ok(read('utils/sparxie/markData.js').includes(`data:image/png;base64,${png}'`), 'markData.js is the PNG of public/images')
  assert.ok(!/['"]\/images\/sparxie-mark/.test(read('utils/sparxie/model.js') + apps + create + game), 'no page points at a file path the host does not serve')
  assert.ok(/Sparxie mark[^]*暗檬[^]*permission of its author/.test(read('public/images/SOURCE.md')))
})

test('the host saves the application itself: creating and importing only read the list again, and the statement is the host\'s own text', () => {
  assert.ok(/onSpxCreated = async \(\{ app, saved \}\)[^]*?else await loadApps\(\)/.test(apps))
  assert.ok(/onScanAdd\(list\)/.test(apps) && apps.includes('await loadApps()'), 'adding from the scan reads the list again')
  assert.ok(notice.includes('state.statement') && notice.includes('C.value.notice'))
  assert.ok(create.includes('failureText(') && COPY.zh.errors.sparxie_running && COPY.zh.errors.config_unreadable && COPY.zh.errors.no_user_session, 'the host\'s error codes become the page\'s own words; a code without a sentence shows the host\'s message')
  const model = read('utils/sparxie/model.js')
  assert.ok(!/display: \[[^\]]*borderless/.test(model), 'the host takes fullscreen and windowed only')
})

test('adding a game follows Sparxie: game tiles, then region, then where the install is, then Sparxie\'s settings, then the game\'s, then the name', () => {
  const t = tpl(create)
  const order = ['C.secGame', 'C.secRegion', 'C.secInstall', '<SparxieOptions', '<SparxieSettings', 'C.secNameCover'].map((k) => t.indexOf(k))
  assert.ok(order.every((n) => n >= 0) && [...order].sort((a, b) => a - b).join() === order.join(), 'the steps come in the order of Sparxie\'s own flow')
  assert.ok(t.includes('v-for="g in matches"') && t.includes('class="m-filterbar spx-search"') && t.includes('v-model="query"'), 'a search box and the list of the titles the host sends')
  assert.ok(!t.includes('GAME_IDS') && create.includes('const titles = computed(') && create.includes('matchTitle(id, { ...catalog.value?.[id], name: names.value[id] }, query.value)'), 'the list is the host\'s, found by name, English name, short name or abbreviation')
  assert.ok(/<MSection v-if="reach\(2\) && !isEdit" :title="C.secRegion">/.test(t) && /<MSection v-if="reach\(3\)" :title="C.secInstall">/.test(t), 'a step shows once the one before it is answered')
  assert.ok(/reach\(4\)[^]*<SparxieOptions[^]*reach\(5\)[^]*<SparxieSettings[^]*reach\(6\)[^]*C\.secNameCover/.test(t), 'Sparxie\'s settings, the game settings and the name wait for an install')
})

test('two ways to add a game that is not found: the executable, or a folder the host searches level by level', () => {
  const t = tpl(create)
  assert.ok(t.includes("pick('file')") && t.includes("pick('folder')") && t.includes('C.pickExe') && t.includes('C.pickFolder'))
  assert.ok(create.includes('client().identify(path, filter)') && create.includes('client().scan(path, filter)'))
  assert.ok(!/client\(\)\.pick\(/.test(create), 'the page never asks the host to open a dialog')
  assert.ok(create.includes('nativePicker()') && create.includes('browsing.value = kind'), 'the panel\'s own dialog where there is one, the host\'s browse list in a browser')
  assert.ok(create.includes("C.value.isSparxie") && create.includes("'is_sparxie'"), 'Sparxie\'s own location is refused with its own words')
})

test('the hero opens under the tiles once the official art has loaded (the tile that was tapped does not move), and its chips do not repeat the launch mode', () => {
  const t = tpl(create)
  assert.ok(/class="spx-hero" :class="\{ 'is-open': heroReady \}"/.test(t) && t.indexOf('spx-hero') > t.indexOf('C.secGame') && t.indexOf('spx-hero') < t.indexOf('C.secRegion'))
  assert.ok(create.includes('probe.onload') && create.includes('heroReady.value = true'))
  assert.ok(/\.spx-hero \{[^}]*grid-template-rows: 0fr[^}]*transition/.test(create) && create.includes('prefers-reduced-motion'), 'it opens by height, and not at all for people who ask for less motion')
  const hero = t.slice(t.indexOf('spx-new__hero'), t.indexOf('</div>', t.indexOf('spx-new__chips')))
  assert.ok(!/C\.touch|C\.pc|modeIcon/.test(hero), 'the launch mode has its own step')
})

test('the 创建 bar is pinned to the bottom of the window as wide as the content column, with the choice on its left and the actions on its right', () => {
  const style = create.slice(create.indexOf('<style'))
  assert.ok(/\.spx-new :deep\(\.m-page__column\) \{ min-height: calc\(100dvh - 96px\); \}/.test(style), 'the column is as tall as the window, so a short page still ends at the bottom edge')
  assert.ok(/\.spx-bar \{[^}]*position: sticky; bottom: 0[^}]*margin: auto 0 -48px/.test(style), 'sticky keeps it there on a long page; the auto margin puts it there on a short one; flush with the end of the page')
  assert.ok(/\.spx-bar \{[^}]*background: var\(--md-sys-color-surface-container-high\)/.test(style), 'opaque: content scrolls under it, never through it')
  const t = tpl(create)
  assert.ok(t.lastIndexOf('class="spx-bar"') > t.lastIndexOf('C.secNameCover') && t.indexOf('spx-bar__sum') < t.indexOf('spx-bar__act'))
  assert.ok(!create.includes('stepsMode') && !create.includes('__SPX_LAYOUT__'), 'one layout: the stepped form was measured against it and lost (3 taps against 8)')
})

test('the bar says what is chosen in one line, says what is open without a sentence, and a blocked 创建 says what is missing on hover and on a long press', () => {
  for (const key of ['barNoGame', 'barNoRegion', 'barNoInstall']) assert.ok(create.includes(`C.value.${key}`), key)
  assert.ok(create.includes("parts.push({ text: install.value.exe || fullPath(install.value) }, { text: touch.value ? inputText.value : C.value.pc })"), 'game · region · executable · input (a title of Sparxie never says 直接启动 while Sparxie is installed)')
  assert.ok(/needGame[^]*needRegion[^]*needInstall/.test(create) && create.includes('role="tooltip"'))
  assert.ok(create.includes('holdStart') && create.includes('420') && /\.spx-bar__go\.is-blocked:hover \.spx-bar__tip, \.spx-bar__go\.is-tip \.spx-bar__tip \{ opacity: 1/.test(create), 'hover and long press show it')
  assert.ok(create.includes('.spx-bar__go.is-blocked :deep(.m-btn) { pointer-events: none; }'), 'a disabled button takes no pointer events: the wrapper answers')
})

test('touch size: the bar\'s buttons are 48 px, the bar keeps clear of the bottom safe area, and on a narrow window the choice takes a row of its own', () => {
  assert.ok(create.includes('.spx-bar :deep(.m-btn) { min-height: 48px; min-width: 48px; }'))
  assert.ok(create.includes('env(safe-area-inset-bottom, 0px)'))
  assert.equal((create.match(/\.spx-bar__sum \{ flex: 1 1 100%; \}/g) || []).length, 2, 'container query and media query')
  assert.ok(create.includes('@container (max-width: 560px)') && create.includes('@media (max-width: 599.98px)'))
})

test('a red error comes only after the person picked something and it failed; a first visit and a failed look by the host say it plainly', () => {
  assert.ok(create.includes("const pickError = ref('')"))
  const errors = [...create.matchAll(/pickError\.value = (?!'')/g)].length
  assert.ok(errors >= 8, 'set in the pick handlers')
  // every place that sets it is a handler of a pick: pick(), onPath(), adopt()
  const before = create.slice(0, create.indexOf('async function pick(kind)'))
  assert.ok(!/pickError\.value = (?!'')/.test(before.replace(/watch\(region[^]*?\}\)/, '')), 'nothing sets it before a pick')
  assert.ok(tpl(create).includes('<MBanner v-else-if="!isEdit && !browsing && !scanning && !pickError" tone="info" icon="info">{{ C.installNone }}</MBanner>'), 'nothing found by the host: a plain info note')
  assert.equal(COPYZH.installNone, '没有自动找到，请选择主程序或文件夹')
})


test('there is no launch choice: a Sparxie title starts through Sparxie while the component is installed, otherwise directly; the input is a setting that hides only what the host marks for touch', () => {
  assert.ok(!/C\.touchVia|chooseMode|modeTouched|C\.secMode/.test(create), 'no 直接启动 / 经 Sparxie control on the page')
  assert.ok(create.includes('const touch = computed(() => touchAllowed.value)'), 'the component and a title Sparxie supports decide, nothing else')
  assert.ok(game.includes('forInput(f, input.value)') && game.includes('f.inputs'), 'the host marks the items for some inputs')
  assert.ok(!/GAME_TRAITS|traitsOf/.test(game + rows + row), 'the page does not name the items that are for touch')
  assert.ok(/\.spx-fold \{[^}]*grid-template-rows: 0fr[^}]*transition/.test(game) && /\.spx-row \{[^}]*grid-template-rows: 0fr[^}]*transition/.test(rows), 'folding eases the height')
  assert.ok(game.includes('prefers-reduced-motion') && rows.includes('prefers-reduced-motion'))
  assert.ok(!/disabled[^\n]*inputs|inputs[^\n]*disabled/.test(rows + row), 'an item for touch only is hidden for the other inputs, never greyed')
})

test('keyboard, mouse and gamepad run through Sparxie when the installed build has the touch-interface switch; otherwise the application starts directly and the page says so once', () => {
  assert.ok(create.includes("const viaSparxie = computed(() => touch.value && (optBlock.options.input !== 'kbm' || (!!status.touchUiSwitch && KBM_SPARXIE_GAMES.includes(game.value))))"), 'the input decides only when the build or the title cannot do kbm')
  assert.ok(create.includes("mode: viaSparxie.value ? 'touch' : 'pc'"), 'the mode follows what the host will do (it applies the same rule)')
  assert.ok(!create.includes("mode: touch.value ? 'touch' : 'pc'"), 'no mode is sent from the bare component presence any more')
  assert.ok(create.includes('<MBanner v-if="!viaSparxie && !status.touchUiSwitch && KBM_SPARXIE_GAMES.includes(game)" tone="info" icon="info">{{ C.kbmNeedsTouch }}</MBanner>'), 'one line under the Sparxie items, only when the build lacks the switch')
  assert.equal(COPYZH.kbmNeedsTouch, '已安装的 Sparxie 无法在键鼠与手柄下保留 PC 界面，所以这些 Sparxie 选项对键鼠与手柄不生效，游戏会由我们直接启动。更新 Sparxie 后即可生效')
  assert.ok(!/直接启动/.test(COPYZH.inputKbm + COPYZH.inputTouch), 'the input names say no launch path')
  assert.ok(create.includes('v-if="reach(4) && touch"'), 'the Sparxie items stay visible under both inputs')
  assert.ok(copy.includes("kbmNeedsTouch: '已安装的 Sparxie 无法在键鼠与手柄下保留 PC 界面") && copy.includes("kbmNeedsTouch: 'The installed Sparxie cannot keep the PC interface"), 'zh and en')
})

test('the game\'s own frame rate is Sparxie\'s while its unlock is on, whichever input is chosen: the item says so and is not written', () => {
  assert.ok(create.includes("const fpsBySparxie = computed(() => viaSparxie.value && !!optBlock.options.fpsUnlock && sparxieFields.value.some((f) => f.key === 'fpsUnlock'))"))
  assert.ok(create.includes('if (fpsBySparxie.value) delete out.fps') && create.includes('settings: settingsBody()'))
  assert.ok(settingsC.includes('props.fpsBySparxie ? { fps: C.value.fpsBySparxie }'))
  assert.ok(copy.includes("fpsBySparxie: '帧率由 Sparxie 接管'"))
})

test('the page has no "cannot read" wording; a game file the host cannot change is one plain line (gsUnsupported) and its items are not drawn', () => {
  assert.ok(!/gsUnavailable|gsRead/.test(settingsC + copy))
  assert.ok(!/尚未写入|读不到/.test(copy.slice(copy.indexOf('const ZH'), copy.indexOf('const EN'))))
  assert.ok(copy.includes('gsUnsupported: (names) => `这个版本的游戏不支持修改：${names}`'))
  assert.ok(settingsC.includes('splitUnavailable'))
})

test('the install row keeps its path readable on a phone: the chips drop below the text instead of squeezing it', () => {
  assert.ok(rowC.includes('<MItem class="spx-pick" wrap'))
})

test('an edit says what failed and why: 保存失败：<reason>, 创建失败：<reason>; every host error code has a sentence, an unknown one shows the host\'s message', async () => {
  const { failureText } = await import('../utils/sparxie/copy.js')
  for (const lang of ['zh', 'en']) {
    const C = COPY[lang]
    assert.equal(failureText(C, C.saveFailed, { code: 'config_conflict', message: 'Sparxie config.json changed while it was being updated' }), C.failedWith(C.saveFailed, C.errors.config_conflict))
    assert.equal(failureText(C, C.createFailed, { code: 'brand_new', message: 'The host says so' }), C.failedWith(C.createFailed, 'The host says so'), 'a code without a sentence shows the host\'s message')
    assert.equal(failureText(C, C.saveFailed, { code: 'failed', message: 'failed' }), C.saveFailed, 'only when nothing was said is it the bare verb')
  }
  assert.equal(failureText(COPYZH, COPYZH.saveFailed, { code: 'apps_write_failed' }), '保存失败：无法保存应用列表')
  // the codes the host can answer with: its sources are the list
  const sources = ['src/sparxie/host.cpp', 'src/sparxie/store.cpp', 'src/sparxie/region.cpp', 'src/sparxie/scan.cpp', 'src/sparxie/settings.cpp', 'src/sparxie/user_session.cpp', 'src/confighttp/sparxie.cpp']
  const codes = new Set()
  for (const f of sources) {
    const path = new URL(`../../../../../${f}`, import.meta.url)
    if (!existsSync(path)) continue
    for (const m of readFileSync(path, 'utf8').matchAll(/(?:Status::fail\(\d+, |"error_code"\] = )"([a-z_]+)"/g)) codes.add(m[1])
  }
  for (const code of codes) if (code !== 'internal_error') for (const lang of ['zh', 'en']) assert.ok(COPY[lang].errors[code], `${lang}: ${code} has a sentence`)
  assert.ok(create.includes('failureText(C.value, isEdit ? C.value.saveFailed : C.value.createFailed, e)'), 'the page says 保存 for an edit and 创建 for a new app')
  assert.ok(!create.includes('CREATE_ERRORS'))
})

test('the launch path follows what the host will do: through Sparxie whenever it can serve the input, else the game starts directly and the page says so once', () => {
  assert.ok(create.includes("const launchMode = computed(() => (viaSparxie.value ? 'touch' : 'pc'))"))
  assert.ok(create.includes('mode: launchMode.value, settings: settingsBody()'), 'the edit sends the launch the host will do')
  assert.ok(create.includes('region: region.value, mode: launchMode.value, name: name.value ||'), 'a new app follows it too')
})

test('the game\'s own frame-rate item says it is the game\'s, and says where the higher rates are when the game stops below 120', () => {
  assert.equal(COPYZH.frameRate, '游戏内帧率上限'); assert.equal(COPY.en.frameRate, 'In-game frame rate limit')
  assert.equal(COPYZH.fpsUnlock, '帧率解锁'); assert.equal(COPYZH.fpsCap, '目标帧率')
  assert.equal(COPYZH.gsFpsHint(60), '游戏本身最高 60 帧，更高的帧率在 Sparxie 设置的「目标帧率」中选择')
  assert.ok(settingsC.includes('props.sparxieFps && !props.fpsBySparxie && top > 0 && top < 120 ? C.value.gsFpsHint(top)') && settingsC.includes("<MFooter v-if=\"fpsHint && g.hasFps\">"), 'the line stands under the group that holds the game\'s frame-rate item')
  assert.ok(create.includes(':sparxie-fps="sparxieFpsAvailable"'))
})

test('the poster badge: a switch under the cover choice, for touch applications only, off by default for a custom cover, saved with the application', () => {
  assert.ok(tpl(cover).includes('<MSwitchItem v-if="touch" v-model="block.badge"') && tpl(cover).includes('C.posterBadge'), 'the switch is in the cover group and only for touch')
  assert.ok(cover.includes('touch: { type: Boolean, default: false }'))
  assert.ok(/function setFollow\(on\) \{\s*block\.cover = on \? 'official' : 'custom'\s*block\.badge = on\s*\}/.test(cover), 'a custom cover starts without the badge, the official poster with it')
  assert.ok(create.includes(`<SparxieCover :form="coverForm" :touch="badgeOn"`), 'the switch is shown for a touch input only')
  assert.ok(create.includes("const badgeOn = computed(() => badgeApplies(launchMode.value, optBlock.options.input))"))
  assert.ok(create.includes("sparxie: { cover: 'official', badge: true }"), 'on for a new application')
  assert.ok(create.includes('coverForm.sparxie.badge = badgeOf(b)'), 'an application opens with what it stored (or the default)')
  assert.ok(create.includes("if (badgeOn.value) body.badge = coverForm.sparxie.badge"), 'saving sends the setting for an application with the touch input')
  assert.ok(create.includes('badge: coverForm.sparxie.badge, image:'), 'creating sends it too')
  assert.ok(create.includes("(b.cover === 'custom' && b.customImage) || props.edit['image-path']"), 'the editor shows the person\'s own image, not the badged copy')
  assert.ok(/badge: badgeOn\.value \? coverForm\.sparxie\.badge : null/.test(create), 'a change of the switch is a change to save')
})
