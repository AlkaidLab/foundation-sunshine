import test from 'node:test'
import assert from 'node:assert/strict'
import { createI18n } from 'vue-i18n'
import en from '../public/assets/locale/en.json' with { type: 'json' }
import zh from '../public/assets/locale/zh.json' with { type: 'json' }
import { validateField, validateAppForm, validateFile } from '../utils/validation.js'
import { FileSelector } from '../utils/fileSelection.js'

test('validation follows the current locale and preserves custom messages', () => {
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en, zh } }).global
  assert.equal(validateField('appName', '', {}, i18n.t).message, en.apps.validation.required)
  i18n.locale.value = 'zh'
  assert.equal(validateField('appName', '', {}, i18n.t).message, zh.apps.validation.required)
  assert.equal(validateField('appName', 'Invalid/name?', {}, i18n.t).message, zh.apps.validation.app_name)
  assert.equal(validateField('command', 'x'.repeat(1001), {}, i18n.t).message, '最多允许 1000 个字符。')
  assert.equal(validateField('appName', '', { message: 'Custom error' }, i18n.t).message, 'Custom error')
  assert.equal(validateField('appName', 'Valid app', {}, i18n.t).isValid, true)
})

test('form errors use localized field labels and command indexes', () => {
  const { t } = createI18n({ legacy: false, locale: 'zh', messages: { zh } }).global
  const result = validateAppForm({
    name: '',
    'prep-cmd': [{ do: '', undo: '' }],
    'menu-cmd': [{ name: '', cmd: '' }],
  }, t)
  assert.equal(result.isValid, false)
  assert.deepEqual(result.errors, [
    `${zh.apps.app_name}: ${zh.apps.validation.required}`,
    '准备命令 1：启动或退出命令至少需要填写一个。',
    '菜单命令 1：显示名称不能为空。',
    '菜单命令 1：命令不能为空。',
  ])
})

test('file validation has an English fallback and localized size limits', () => {
  const { t } = createI18n({ legacy: false, locale: 'zh', messages: { zh } }).global
  assert.equal(validateFile(null).message, 'Select a file.')
  assert.equal(validateFile({ type: 'image/png', size: 11 * 1024 * 1024 }, { translate: t }).message,
    '文件大小不能超过 10.0 MB。')
  assert.equal(validateFile({ type: 'image/png', size: 1024 }).isValid, true)
})

test('native dialog titles, filters and notifications follow locale changes', async (context) => {
  const originalWindow = globalThis.window
  context.after(() => {
    if (originalWindow === undefined) delete globalThis.window
    else globalThis.window = originalWindow
  })
  const i18n = createI18n({ legacy: false, locale: 'en', messages: { en, zh } }).global
  const notifications = []
  const dialogs = []
  globalThis.window = {
    __TAURI__: {
      dialog: {
        open: async options => {
          dialogs.push(options)
          return options.directory ? 'C:\\Games' : 'C:\\Games\\app.exe'
        },
      },
    },
  }
  const selector = new FileSelector({ translate: i18n.t, onSuccess: message => notifications.push(message) })
  const paths = []
  await selector.selectFile('cmd', null, (field, selected) => paths.push([field, selected]))
  assert.equal(dialogs[0].title, 'Select a file')
  assert.deepEqual(dialogs[0].filters.map(filter => filter.name), ['Executable files', 'All files'])
  assert.deepEqual(paths, [['cmd', 'C:\\Games\\app.exe']])
  assert.equal(notifications[0], 'File selected: C:\\Games\\app.exe')
  i18n.locale.value = 'zh'
  await selector.selectDirectory('working-dir', null, (field, selected) => paths.push([field, selected]))
  assert.equal(dialogs[1].title, '选择目录')
  assert.equal(notifications[1], '已选择目录：C:\\Games')
  assert.equal(selector.currentField, null)
})
