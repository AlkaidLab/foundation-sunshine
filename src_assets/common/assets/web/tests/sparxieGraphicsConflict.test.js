import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { copyFor } from '../utils/sparxie/copy.js'
import { askGraphicsConflict, graphicsConflict, resolveGraphicsConflict } from '../utils/sparxie/graphicsConflict.js'

test('a menu choice and an argument for the other DirectX are a conflict, in each title that has both', () => {
  assert.deepEqual(graphicsConflict('genshin', { dxVersion: 'dx12', customArgs: '-a -force-d3d11' }), { key: 'dxVersion', value: 'dx12', args: ['-force-d3d11'] })
  assert.deepEqual(graphicsConflict('starrail', { dxVersion: 'dx11', customArgs: '"-force-d3d12"' }).args, ['-force-d3d12'])
  assert.deepEqual(graphicsConflict('zzz', { graphicsApi: '11.0', customArgs: '-use-d3d12' }), { key: 'graphicsApi', value: '11.0', args: ['-use-d3d12'] })
  assert.deepEqual(graphicsConflict('hi3', { graphicsApi: '12', customArgs: '-a\n-FORCE-D3D11' }).args, ['-FORCE-D3D11'])
})

test('no choice in the menu, the same DirectX twice, or a title without both is no conflict', () => {
  for (const [game, settings] of [
    ['genshin', { dxVersion: 'auto', customArgs: '-force-d3d11' }],
    ['genshin', { customArgs: '-force-d3d11' }],
    ['genshin', { dxVersion: 'dx12', customArgs: '-force-d3d12 -mine' }],
    ['zzz', { graphicsApi: '12', customArgs: '-force-d3d12' }],
    ['zzz', { graphicsApi: '11.1ns', customArgs: '-force-d3d11' }],
    ['genshin', { dxVersion: 'dx12' }],
    ['endfield', { dxVersion: 'dx12', customArgs: '-force-d3d11' }],
  ]) assert.equal(graphicsConflict(game, settings), null, `${game} ${JSON.stringify(settings)}`)
})

test('turning the setting off keeps the arguments; removing the arguments keeps the rest as typed', () => {
  const typed = { dxVersion: 'dx12', customArgs: '-a   -b\n-force-d3d11  -c\n"-force-d3d11""-d"' }
  const menu = { ...typed }
  resolveGraphicsConflict(menu, graphicsConflict('genshin', menu), 'menu')
  assert.deepEqual(menu, { customArgs: typed.customArgs })
  const args = { ...typed }
  resolveGraphicsConflict(args, graphicsConflict('genshin', args), 'args')
  assert.deepEqual(args, { dxVersion: 'dx12', customArgs: '-a   -b\n-c\n"-d"' })
  // nothing left: the item is not held any more
  const only = { graphicsApi: '12', customArgs: '-force-d3d11' }
  resolveGraphicsConflict(only, graphicsConflict('zzz', only), 'args')
  assert.deepEqual(only, { graphicsApi: '12' })
})

test('the dialog has the two answers; closing it is neither of them', async () => {
  const conflict = { key: 'dxVersion', value: 'dx12', args: ['-force-d3d11'] }
  let shown = null
  const answer = (action) => async (options) => {
    shown = options
    if (action !== 'confirm') throw action
    return action
  }
  assert.equal(await askGraphicsConflict(conflict, 'DirectX 12', copyFor('zh'), { ask: answer('confirm') }), 'menu')
  assert.deepEqual([shown.title, shown.message, shown.confirmText, shown.cancelText, shown.distinguish],
    ['图形 API 冲突', '图形 API 选了「DirectX 12」，自定义启动参数里又有 -force-d3d11。停用图形 API 选项，还是删除参数？', '停用图形 API 选项', '删除参数', true])
  assert.equal(await askGraphicsConflict(conflict, 'DirectX 12', copyFor('en'), { ask: answer('cancel') }), 'args')
  assert.equal(await askGraphicsConflict(conflict, 'DirectX 12', copyFor('en'), { ask: answer('close') }), null)
})

test('the page asks before it saves, for a new application and for an existing one', () => {
  const page = readFileSync(new URL('../components/sparxie/SparxieNewApp.vue', import.meta.url), 'utf8')
  const create = page.slice(page.indexOf('async function create()'))
  const asked = create.indexOf('if (!await settleGraphicsApi()) return')
  assert.ok(asked > 0 && asked < create.indexOf('updateApp(') && asked < create.indexOf('createApp('))
})
