import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const WEB = join(dirname(fileURLToPath(import.meta.url)), '..')
const page = readFileSync(join(WEB, 'configs/tabs/display/DisplayOutput.vue'), 'utf8')

test('息屏串流就绪状态由统一的 computed 判定：虚拟显示器 + 仅使用此显示器 + 整屏捕获', () => {
  assert.match(page, /const screenoffReady = computed\(\(\) => chosen\.value === 'virtual' && prepModel\.value === 'ensure_only_display' && \(props\.config\.capture_target \|\| 'display'\) === 'display'\)/)
})

test('对话框只在点击「配置息屏串流」时打开，页面加载不会自动弹出', () => {
  assert.match(page, /const screenoffAsk = ref\(false\)/)
  assert.equal((page.match(/screenoffAsk = true/g) || []).length, 1, '只有入口按钮这一处打开对话框')
  assert.doesNotMatch(page, /onMounted[\s\S]*?screenoffAsk[\s\S]*?= true/)
})

test('点击「切换配置」通过响应式 setter 写入 output_name、display_device_prep，必要时补 capture_target', () => {
  assert.match(page, /const applyScreenoff = \(\) => \{\s*\n\s*chosen\.value = 'virtual'\s*\n\s*prepModel\.value = 'ensure_only_display'\s*\n\s*if \(\(props\.config\.capture_target \|\| 'display'\) !== 'display'\) captureTarget\.value = 'display'/)
})

test('「暂不切换」只关闭对话框，不写入任何配置', () => {
  assert.match(page, /<MButton variant="text" @click="screenoffAsk = false">\{\{ t\('config\.screenoff_dialog_cancel'\) \}\}<\/MButton>/)
})

test('卡片文案与状态随 screenoffReady 切换，已就绪时不再显示配置按钮', () => {
  assert.match(page, /:text="screenoffReady \? t\('config\.screenoff_status_ready'\) : t\('config\.screenoff_status_todo'\)"/)
  assert.match(page, /<MInfo :text="screenoffReady \? t\('config\.screenoff_ready_text'\) : t\('config\.screenoff_todo_text'\)" \/>/)
  assert.match(page, /<MButton v-if="!screenoffReady" variant="tonal" size="s" @click="screenoffAsk = true">/)
})

test('切换后弹出轻提示告知保存后生效', () => {
  assert.match(page, /<MSnackbar v-model="screenoffSnack" :message="t\('config\.screenoff_applied'\)" \/>/)
  assert.match(page, /screenoffSnack\.value = true/)
})

test('对话框不使用浏览器原生 alert/confirm', () => {
  assert.doesNotMatch(page, /\b(?:window\.)?(?:alert|confirm)\(/)
})

test('对话框模板里的 displayTreeTexts 不带 .value（模板自动解包，带上会在渲染时抛错）', () => {
  const template = page.slice(page.indexOf('<template>'))
  assert.doesNotMatch(template, /x\.value\./)
  assert.match(template, /\{\{ screenoffOutputNow \}} → \{\{ x\.display\.virtual \}\}/)
  assert.match(template, /\{\{ x\.prep\[prepModel\] \}} → \{\{ x\.prep\.ensure_only_display \}\}/)
  assert.doesNotMatch(template, /prepModel\.value \}\}/)
})

test('息屏串流向导的中英文文案齐全，旧组合提示键已移除', () => {
  const en = JSON.parse(readFileSync(join(WEB, 'public/assets/locale/en.json'), 'utf8'))
  const zh = JSON.parse(readFileSync(join(WEB, 'public/assets/locale/zh.json'), 'utf8'))
  const keys = ['screenoff_action', 'screenoff_applied', 'screenoff_card_title', 'screenoff_dialog_body', 'screenoff_dialog_cancel', 'screenoff_dialog_confirm', 'screenoff_dialog_prep', 'screenoff_dialog_risk', 'screenoff_dialog_title', 'screenoff_ready_text', 'screenoff_status_ready', 'screenoff_status_todo', 'screenoff_todo_text']
  for (const key of keys) assert.ok(en.config[key] && zh.config[key], 'missing config.' + key)
  assert.equal(en.config.screenoff_combo_hint, undefined)
  assert.equal(zh.config.screenoff_combo_hint, undefined)
})
