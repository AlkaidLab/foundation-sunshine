import test from 'node:test'
import assert from 'node:assert/strict'

import { AppService } from '../services/appService.js'

test('searchApps returns a shallow copy when query is empty', () => {
  const apps = [
    { name: 'Steam Big Picture', cmd: 'steam://open/bigpicture' },
    { name: 'Moonlight', cmd: 'moonlight.exe' },
  ]

  const result = AppService.searchApps(apps, '   ')

  assert.deepEqual(result, apps)
  assert.notEqual(result, apps)
})

test('searchApps matches app name and command case-insensitively', () => {
  const apps = [
    { name: 'Steam Big Picture', cmd: 'steam://open/bigpicture' },
    { name: 'RetroArch', cmd: 'C:/Games/retroarch.exe' },
    { name: 'Desktop', cmd: 'mstsc.exe' },
  ]

  assert.deepEqual(AppService.searchApps(apps, 'retro'), [apps[1]])
  assert.deepEqual(AppService.searchApps(apps, 'MSTSC'), [apps[2]])
})

test('restoreDefaultBuiltInApps restores matching defaults and appends missing apps', () => {
  const customApp = { name: 'Moonlight', cmd: 'moonlight.exe' }
  const result = AppService.restoreDefaultBuiltInApps([
    { name: 'Desktop', cmd: 'broken.exe', index: 0 },
    { name: 'Steam Big Picture', cmd: 'steam://open/bigpicture', 'auto-detach': 'true', 'wait-all': 'true', 'image-path': 'steam.png' },
    customApp,
  ], 'windows')

  assert.equal(result.restored, 1)
  assert.equal(result.added, 1)
  assert.equal(result.changed, 2)
  assert.deepEqual(result.apps[0], AppService.getDefaultBuiltInApps('windows')[0])
  assert.deepEqual(result.apps[2], customApp)
  assert.equal(result.apps.at(-1).name, 'Xbox Game')
})

test('restoreDefaultBuiltInApps leaves unknown platforms unchanged', () => {
  const apps = [{ name: 'Moonlight', cmd: 'moonlight.exe' }]
  const result = AppService.restoreDefaultBuiltInApps(apps, 'unknown')

  assert.equal(result.changed, 0)
  assert.deepEqual(result.apps, apps)
  assert.notEqual(result.apps, apps)
})

test('formatAppData trims scalar fields and removes empty prep commands', () => {
  const result = AppService.formatAppData({
    name: '  Game  ',
    output: '  monitor-1  ',
    cmd: '  game.exe  ',
    elevated: 1,
    'auto-detach': false,
    'wait-all': true,
    'exit-timeout': '12',
    'prep-cmd': [
      { do: '  ', undo: '' },
      { do: 'start-service', undo: '' },
      { do: '', undo: 'stop-service' },
    ],
    'menu-cmd': 'not-an-array',
    detached: ['helper.exe'],
    'image-path': '  cover.png  ',
    'working-dir': '  C:/Games/Game  ',
  })

  assert.equal(result.name, 'Game')
  assert.equal(result.output, 'monitor-1')
  assert.equal(result.cmd, 'game.exe')
  assert.equal(result.elevated, true)
  assert.equal(result['exit-timeout'], 12)
  assert.deepEqual(result['prep-cmd'], [
    { do: 'start-service', undo: '' },
    { do: '', undo: 'stop-service' },
  ])
  assert.deepEqual(result['menu-cmd'], [])
  assert.deepEqual(result.detached, ['helper.exe'])
  assert.equal(result['image-path'], 'cover.png')
  assert.equal(result['working-dir'], 'C:/Games/Game')
})
