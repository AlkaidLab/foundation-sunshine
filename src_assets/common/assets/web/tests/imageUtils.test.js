import test from 'node:test'
import assert from 'node:assert/strict'

import { getImagePreviewUrl } from '../utils/imageUtils.js'

test('getImagePreviewUrl keeps direct preview URLs unchanged', () => {
  assert.equal(getImagePreviewUrl('https://cdn.example.com/cover.jpg'), 'https://cdn.example.com/cover.jpg')
  assert.equal(getImagePreviewUrl('data:image/png;base64,abc'), 'data:image/png;base64,abc')
})

test('getImagePreviewUrl normalizes boxart and cover file names', () => {
  assert.equal(getImagePreviewUrl('Half Life.png'), '/boxart/Half%20Life.png')
  assert.equal(getImagePreviewUrl('Half%20Life.png'), '/boxart/Half%20Life.png')
  assert.equal(getImagePreviewUrl('boxart/Half%20Life.png'), '/boxart/Half%20Life.png')
})

test('getImagePreviewUrl maps localized covers paths to boxart URLs', () => {
  assert.equal(
    getImagePreviewUrl('C:\\Users\\player\\AppData\\Local\\Sunshine\\covers\\Half Life.png'),
    '/boxart/Half%20Life.png'
  )
  assert.equal(
    getImagePreviewUrl('/home/player/.config/sunshine/covers/Half%20Life.png'),
    '/boxart/Half%20Life.png'
  )
})
