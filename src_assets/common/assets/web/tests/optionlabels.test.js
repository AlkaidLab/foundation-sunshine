import test from 'node:test'
import assert from 'node:assert/strict'
import { optionLabelOf, registerOptionLabels } from '../composables/optionLabels.js'

test('the words a page shows for the options of a setting are what the unsaved list shows, the zero option included', () => {
  registerOptionLabels('lan_encryption_mode', [{ value: 0, label: '不加密' }, { value: 1, label: '为支持的客户端启用' }])
  assert.equal(optionLabelOf('lan_encryption_mode', 0), '不加密')
  assert.equal(optionLabelOf('lan_encryption_mode', '1'), '为支持的客户端启用')
  assert.equal(optionLabelOf('lan_encryption_mode', 5), undefined)
  assert.equal(optionLabelOf('no_such_key', 0), undefined)
})
