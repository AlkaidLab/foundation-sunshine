// A settings page that only just fills a 1440x900 window must not scroll for a few pixels: 主机设置 › 常规 is 856 px with the default 48 px tail of .m-page in a 852 px pane.
import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'

const less = readFileSync(join(dirname(fileURLToPath(import.meta.url)), '../styles/m3e-web.less'), 'utf8')

test('the settings pages end 40 px under their last row, so 主机设置 › 常规 (808 px of content) fits a 852 px pane', () => {
  const m = /\.cfg-page \.m-page \{\s*padding-bottom:\s*(\d+)px;/.exec(less)
  assert.ok(m, 'a rule sets the tail of .m-page inside .cfg-page')
  const tail = +m[1]
  assert.ok(808 + tail <= 852, `${808 + tail} px of page in a 852 px pane scrolls`)
  assert.ok(tail >= 32, `a tail of ${tail} px leaves no clear gap under the last row`)
})
