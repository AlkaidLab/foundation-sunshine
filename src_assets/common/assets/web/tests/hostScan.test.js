import test from 'node:test'
import assert from 'node:assert/strict'
import { mapHostGame, readScan, scanBody } from '../utils/hostScan.js'

test('the host scan asks for the chosen sources and the directory, nothing else', () => {
  assert.deepEqual(scanBody({ platforms: ['steam', 'epic', 'steam', 'nope'] }), { sources: ['steam', 'epic'] })
  assert.deepEqual(scanBody({ directory: ' D:\\Games ' }), { directory: 'D:\\Games' })
  assert.deepEqual(scanBody({ platforms: ['gog'], directory: 'E:\\x' }), { sources: ['gog'], directory: 'E:\\x' })
  assert.deepEqual(scanBody({}), {})
})

test('a game of the answer becomes an application of the result sheet', () => {
  const steam = { platform: 'steam', name: 'Portal 2', id: '620', install_dir: 'D:\\SteamLibrary\\steamapps\\common\\Portal 2', launch: 'steam://rungameid/620', exe: 'D:\\SteamLibrary\\steamapps\\common\\Portal 2\\portal2.exe', cover_hint: 'https://cdn.example/620.jpg' }
  assert.deepEqual(mapHostGame(steam), { name: 'Portal 2', cmd: 'steam://rungameid/620', 'working-dir': steam.install_dir, 'image-path': 'https://cdn.example/620.jpg', source_path: steam.install_dir, 'app-type': 'steam', 'is-game': true, exe: steam.exe, sources: ['steam'] })
  assert.equal(mapHostGame({ platform: 'directory', name: 'Tool', id: 'C:\\g\\tool.exe', exe: 'C:\\g\\tool.exe' }).cmd, 'C:\\g\\tool.exe')
  assert.equal(mapHostGame({ platform: 'gog', name: 'W3', id: '1', launch: '"C:\\GOG\\w3.exe"' })['image-path'], '')
})

test('the answer: counts per platform and warnings; 404 and 501 mean the host has no scan; 409 means one is running', () => {
  const ok = readScan(200, { games: [{ platform: 'steam', name: 'A' }, { platform: 'steam', name: 'B' }, { platform: 'epic', name: 'C' }], warnings: ['gog unavailable'] })
  assert.equal(ok.ok, true); assert.deepEqual(ok.counts, { steam: 2, epic: 1 }); assert.equal(ok.games.length, 3); assert.deepEqual(ok.warnings, ['gog unavailable'])
  assert.deepEqual(readScan(404, null), { ok: false, unsupported: true, status: 404 })
  assert.equal(readScan(501, null).unsupported, true)
  assert.equal(readScan(409, {}).busy, true)
  assert.deepEqual(readScan(400, { error: 'bad directory' }), { ok: false, status: 400, error: 'bad directory' })
  assert.deepEqual(readScan(200, { games: [] }).games, [])
  assert.equal(readScan(200, {}).ok, false)
})
