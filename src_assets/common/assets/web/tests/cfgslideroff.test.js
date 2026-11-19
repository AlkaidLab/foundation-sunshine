import { test } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

// Sliders whose lowest position is "automatic" name that state inside the track (MSlider off-text), so the empty bar is not mistaken for a broken control.
const read = (f) => fs.readFileSync(new URL(`../${f}`, import.meta.url), 'utf8')

test('the expert slider passes the name of its off value, and the bitrate slider says 自动 at zero', () => {
  assert.match(read('components/cfg/CfgExpert.vue'), /:off-text="offValue !== undefined && !stepList \? fmt\(lo\) : ''"/)
  assert.match(read('configs/tabs/audiovideo/BitrateControl.vue'), /<MSlider v-model="progress"[^>]*:off-text="\$t\('_common\.auto'\)"/)
})
