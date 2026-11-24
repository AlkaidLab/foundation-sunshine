import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'
import { osFromLog, saveBundle } from '../utils/diagnosticsFiles.js'

const here = dirname(fileURLToPath(import.meta.url))
const web = join(here, '..')

test('the system line comes from the host log', () => {
  assert.equal(osFromLog('[2026-11-02 08:00:01.130]: Info: Running with Windows 11 Pro 10.0.26100 x64\n[2026-11-02 08:00:02.000]: Info: other'), 'Windows 11 Pro 10.0.26100 x64')
  assert.equal(osFromLog(''), '')
  assert.equal(osFromLog('Running with nothing'), '')
})

test('the bundle goes through the control panel save dialog when there is one, and a cancel stays a cancel', async () => {
  const calls = []
  const win = { __TAURI_INTERNALS__: { invoke: async (cmd, args) => { calls.push([cmd, args]); return 'C:\\out\\d.zip' } } }
  assert.equal(await saveBundle('d.zip', new Uint8Array([1, 2, 3]), win), 'C:\\out\\d.zip')
  assert.deepEqual(calls[0], ['save_binary_file', { contentBase64: 'AQID', defaultName: 'd.zip', filterName: 'Zip', extensions: ['zip'] }])
  await assert.rejects(saveBundle('d.zip', new Uint8Array([1]), { __TAURI_INTERNALS__: { invoke: async () => { throw 'cancelled' } } }), /cancelled/)
})

test('a real bundle write failure is reported with its reason, not turned into a download', async () => {
  const win = { __TAURI_INTERNALS__: { invoke: async () => { throw 'write failed: disk full' } } }
  await assert.rejects(saveBundle('d.zip', new Uint8Array([1]), win), /write failed: disk full/)
})

test('every link of the about page is one of the README or resources.js of this repository', async (t) => {
  const dataFile = process.env.FOUNDATION_UI_DIR ? join(process.env.FOUNDATION_UI_DIR, 'src/about/data.js') : join(here, '../../../../../../ds5-ui-m3e/src/about/data.js')
  let data
  try { data = await import(dataFile) } catch { t.skip('foundation-ui is not next to this checkout'); return }
  const { PROJECT_LINKS, THANKS } = data
  const root = join(here, '../../../../..')
  const readme = readFileSync(join(root, 'README.md'), 'utf8')
  const resources = readFileSync(join(web, 'config/resources.js'), 'utf8')
  const urls = [...PROJECT_LINKS.flatMap((l) => [l.url, l.zh]), ...THANKS.map((t) => t.url)].filter(Boolean)
  const known = (url) => readme.includes(url) || resources.includes(url) || readme.includes(url.replace(/#readme$/, ''))
    || /^https:\/\/github\.com\/AlkaidLab\/foundation-sunshine(\/(releases|issues|blob\/master\/LICENSE))?$/.test(url) || /^https:\/\/www\.alkaid(lab\.(com|cn))\/docs\/$/.test(url)
  for (const url of urls) assert.ok(known(url), `${url} is not in README.md or resources.js`)
})
