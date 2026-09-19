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
    gamepad: 'x360',
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
  assert.equal(result.gamepad, 'x360')
  assert.deepEqual(result['prep-cmd'], [
    { do: 'start-service', undo: '' },
    { do: '', undo: 'stop-service' },
  ])
  assert.deepEqual(result['menu-cmd'], [])
  assert.deepEqual(result.detached, ['helper.exe'])
  assert.equal(result['image-path'], 'cover.png')
  assert.equal(result['working-dir'], 'C:/Games/Game')
})

test('formatAppData rejects unknown per-app gamepad values', () => {
  const result = AppService.formatAppData({
    name: 'Game',
    gamepad: 'invalid',
  })

  assert.equal(result.gamepad, '')
})

test('formatAppData preserves a per-app DualSense override', () => {
  const result = AppService.formatAppData({
    name: 'Game',
    gamepad: 'ds5',
  })

  assert.equal(result.gamepad, 'ds5')
})

test('formatAppData normalizes per-app RTX HDR settings', () => {
  const result = AppService.formatAppData({
    name: 'Game',
    'rtx-hdr': {
      mode: 'on',
      contrast: 140,
      saturation: -120,
      'middle-gray': 70,
      'peak-nits': 850,
    },
  })

  assert.deepEqual(result['rtx-hdr'], {
    mode: 'on',
    contrast: 100,
    saturation: -100,
    'middle-gray': 70,
    'peak-nits': 850,
  })
})

test('editing an app preserves DLSS NR settings, including zero intensity', () => {
  const dlssnr = {
    mode: 'on', style: 3, 'motion-quality': 0, intensity: 0,
    'local-tone-strength': 0.25, 'local-structure-strength': 0.5,
    'skin-structure-strength': 0, 'auto-mask': true, 'ui-correction': true,
  }
  const saved = AppService.formatAppData({ name: 'Genshin', dlssnr })
  assert.deepEqual(saved.dlssnr, dlssnr)
  assert.deepEqual(AppService.formatAppData(saved).dlssnr, dlssnr)
  assert.notEqual(saved.dlssnr, dlssnr)
})

test('DLSS NR stays disabled by default and normalizes invalid input', () => {
  assert.equal(AppService.formatAppData({ name: 'Game' }).dlssnr.mode, 'inherit')
  const saved = AppService.formatAppData({ name: 'Game', dlssnr: {
    mode: 'invalid', style: 99, intensity: -1,
    'local-tone-strength': 'invalid', 'skin-structure-strength': Infinity,
    'auto-mask': 'false', 'ui-correction': 'true',
  } }).dlssnr
  assert.equal(saved.mode, 'inherit')
  assert.equal(saved.style, 4)
  assert.equal(saved.intensity, 0)
  assert.equal(saved['local-tone-strength'], 1)
  assert.equal(saved['skin-structure-strength'], 0)
  assert.equal(saved['auto-mask'], false)
  assert.equal(saved['ui-correction'], true)
})
