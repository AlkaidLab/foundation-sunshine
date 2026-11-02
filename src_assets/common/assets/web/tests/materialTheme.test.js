import assert from 'node:assert/strict'
import test from 'node:test'
import { argbFromHex } from '@material/material-color-utilities'
import { materialSeedFromImageData, materialThemeTokens } from '../utils/materialTheme.js'

const rgb = (hex) => [1, 3, 5].map((index) => parseInt(hex.slice(index, index + 2), 16))
const luminance = (hex) =>
  rgb(hex)
    .map((channel) => {
      const value = channel / 255
      return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4
    })
    .reduce((sum, channel, index) => sum + channel * [0.2126, 0.7152, 0.0722][index], 0)
const contrast = (a, b) => {
  const values = [luminance(a), luminance(b)].sort((x, y) => y - x)
  return (values[0] + 0.05) / (values[1] + 0.05)
}

test('solid wallpaper seeds are stable and ignore transparent pixels', () => {
  const visible = [36, 145, 100, 255, 36, 145, 100, 255, 36, 145, 100, 255]
  const seed = materialSeedFromImageData({ data: visible })
  assert.equal(materialSeedFromImageData({ data: [...visible, 255, 0, 0, 0] }), seed)
  for (let index = 0; index < 3; index++) {
    assert.equal(materialSeedFromImageData({ data: visible }), seed)
  }
})

test('empty and fully transparent wallpaper samples keep the normal theme', () => {
  assert.equal(materialSeedFromImageData({ data: [] }), null)
  assert.equal(materialSeedFromImageData({ data: [255, 0, 0, 0] }), null)
})

test('light and dark palettes maintain readable content tones for diverse seeds', () => {
  for (const seedHex of ['#4a4b79', '#e5507a', '#159557', '#0078b4', '#ffffff', '#000000']) {
    for (const dark of [false, true]) {
      const tokens = materialThemeTokens(argbFromHex(seedHex), dark)
      for (const text of ['--mt-text-primary', '--mt-text-secondary']) {
        assert.ok(contrast(tokens[text], tokens['--mt-page-bg']) >= 4.5, `${seedHex} ${dark} ${text}`)
      }
    }
  }
})

test('tokens stay scoped to home content and never claim brand variables', () => {
  for (const dark of [false, true]) {
    const tokens = materialThemeTokens(argbFromHex('#e5507a'), dark)
    for (const name of Object.keys(tokens)) {
      assert.ok(
        name === '--wallpaper-seed' || name.startsWith('--mt-'),
        `${name} must not be a brand variable`,
      )
    }
    assert.equal(Object.keys(tokens).length, 4)
  }
})

test('appearance changes preserve the wallpaper seed and select different tonal roles', () => {
  const seed = argbFromHex('#159557')
  const light = materialThemeTokens(seed, false)
  const dark = materialThemeTokens(seed, true)
  assert.equal(light['--wallpaper-seed'], dark['--wallpaper-seed'])
  assert.notEqual(light['--mt-page-bg'], dark['--mt-page-bg'])
  assert.ok(luminance(light['--mt-page-bg']) > luminance(dark['--mt-page-bg']))
})
