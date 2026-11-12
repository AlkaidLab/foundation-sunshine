import test from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

// one name for one thing: the theme button of the narrow window is 个性化 (its menu: light, dark, follow the system, and 个性化 itself); beside the rail's own 个性化 button it is the 明暗 switch
const read = (p) => readFile(new URL(p, import.meta.url), 'utf8')

test('the theme button is named 个性化 where the bar of a phone has no other way to it, and 明暗 beside the rail button', async () => {
  const src = await read('../components/layout/ShellUtils.vue')
  assert.match(src, /const themeTitle = computed\(\(\) => \(props\.placement === 'rail' \? appearanceTexts\(String\(locale\.value\)\)\.mode : appearanceTitle\.value\)\)/)
  assert.match(src, /:label="`\$\{themeTitle\} \(\$\{t\(current\(\)\.label\)\}\)`"/)
  assert.ok(!src.includes("navbar.toggle_theme"), 'no second name (主题) for the same thing')
})

test('the choice that follows the system says so in the words of 个性化', async () => {
  for (const [file, text] of [['zh', '跟随系统'], ['en', 'Follow system'], ['zh_TW', '跟隨系統']]) assert.equal(JSON.parse(await read(`../public/assets/locale/${file}.json`)).navbar.theme_auto, text, file)
})
