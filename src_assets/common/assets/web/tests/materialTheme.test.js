import test from 'node:test'
import assert from 'node:assert/strict'
import { argbFromHex } from '@material/material-color-utilities'
import { materialSeedFromImageData, materialThemeTokens } from '../utils/materialTheme.js'

const luminance = (hex) =>
  [1, 3, 5]
    .map((index) => parseInt(hex.slice(index, index + 2), 16) / 255)
    .map((value) => (value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4))
    .reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0)
const contrast = (a, b) => {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x)
  return (hi + 0.05) / (lo + 0.05)
}

test('seed extraction ignores transparency and rejects empty samples', () => {
  const visible = [36, 150, 100, 255, 36, 150, 100, 255]
  const seed = materialSeedFromImageData({ data: visible })
  assert.equal(materialSeedFromImageData({ data: [...visible, 255, 0, 0, 0] }), seed)
  assert.equal(materialSeedFromImageData({ data: [255, 0, 0, 0] }), null)
  assert.equal(materialSeedFromImageData({ data: [] }), null)
})

test('light and dark palettes keep content tones readable for diverse seeds', () => {
  for (const seedHex of ['#4a4b79', '#e5507a', '#159557', '#0078b4', '#ffffff', '#000000']) {
    for (const dark of [false, true]) {
      const tokens = materialThemeTokens(argbFromHex(seedHex), dark)
      for (const text of ['--mt-text-primary', '--mt-text-secondary']) {
        assert.ok(
          contrast(tokens[text], tokens['--mt-page-bg']) >= 4.5,
          `${seedHex} ${dark} ${text}`,
        )
      }
    }
  }
})

test('tokens stay scoped to home content and never claim brand variables', () => {
  for (const dark of [false, true]) {
    const tokens = materialThemeTokens(argbFromHex('#e5507a'), dark)
    for (const name of Object.keys(tokens)) {
      assert.ok(name.startsWith('--mt-'), `${name} must not be a brand variable`)
    }
    assert.equal(Object.keys(tokens).length, 3)
  }
})

test('appearance changes select different tonal roles', () => {
  const seed = argbFromHex('#159557')
  const light = materialThemeTokens(seed, false)
  const night = materialThemeTokens(seed, true)
  assert.notEqual(light['--mt-page-bg'], night['--mt-page-bg'])
  assert.ok(luminance(light['--mt-page-bg']) > luminance(night['--mt-page-bg']))
})
