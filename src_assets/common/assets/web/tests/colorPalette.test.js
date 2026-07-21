import assert from 'node:assert/strict'
import test from 'node:test'

import { extractColors, rgbToHsl, selectAccentColor } from '../utils/colorPalette.js'

const imageDataFrom = (pixels) => ({
  data: Uint8ClampedArray.from(pixels.flatMap(([r, g, b, a = 255]) => [r, g, b, a])),
})

const isNearColor = (color, expected, tolerance = 2) =>
  color.every((channel, index) => Math.abs(channel - expected[index]) <= tolerance)

test('extractColors clusters sampled pixels with desktop-style K-means', () => {
  const imageData = imageDataFrom([
    [255, 0, 0],
    [250, 5, 0],
    [255, 10, 0],
    [245, 0, 5],
    [0, 0, 255],
    [0, 5, 250],
    [10, 0, 255],
    [0, 10, 245],
  ])

  const colors = extractColors(imageData, 2, () => 0.5)

  assert.equal(colors.length, 2)
  assert(colors.some((color) => isNearColor(color, [251, 4, 1])))
  assert(colors.some((color) => isNearColor(color, [2, 4, 251])))
})

test('extractColors ignores transparent pixels', () => {
  const imageData = imageDataFrom([
    [255, 0, 0, 0],
    [0, 255, 0, 255],
    [0, 0, 255, 255],
  ])

  const colors = extractColors(imageData, 3)

  assert.equal(colors.length, 2)
  assert(colors.every((color) => !isNearColor(color, [255, 0, 0])))
})

test('extractColors handles empty images and invalid cluster counts', () => {
  assert.deepEqual(extractColors({ data: new Uint8ClampedArray() }, 3), [])
  assert.deepEqual(extractColors(imageDataFrom([[255, 0, 0]]), 0), [])
})

test('selectAccentColor prefers saturated colors with usable lightness', () => {
  assert.deepEqual(selectAccentColor([[255, 255, 255], [0, 128, 255], [128, 128, 128]]), [0, 128, 255])
})

test('selectAccentColor returns null for empty or invalid palettes', () => {
  assert.equal(selectAccentColor(), null)
  assert.equal(selectAccentColor([null, undefined, []]), null)
})

test('rgbToHsl returns the expected hue, saturation, and lightness', () => {
  assert.deepEqual(rgbToHsl(255, 0, 0).map(Math.round), [0, 100, 50])
})
