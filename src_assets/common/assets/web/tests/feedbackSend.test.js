import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const read = (p) => fs.readFileSync(path.join(here, '..', p), 'utf8')

test('the Web UI mounts the one report surface outside an embedded frame, with the logs in the report context', () => {
  const shell = read('views/WebShell.vue')
  assert.match(shell, /<MErrorHost v-if="!embedded"[^\n]*:open-url=/)
  assert.match(shell, /logs: String\(recentLogText\.value \|\| ''\)\.split\('\\n'\)\.slice\(-40\)\.join\('\\n'\)/)
  assert.match(shell, /startAutoReport\(\{ context: errorContext \}\); void refreshLogs\(\)/)
})

test('Troubleshooting is the shared diagnostics page: its feedback row sends through MErrorHost, there is no private path', () => {
  const page = read('views/Troubleshooting.vue')
  assert.match(page, /<MDiagnostics\b/); assert.doesNotMatch(page, /issues\/new/)
})

test('the Content-Security-Policy of the host pages does not limit where a page may connect (only who may frame them), so the Firestore request is not blocked', () => {
  const dir = path.join(here, '..', '..', '..', '..', '..', 'src', 'confighttp')
  const src = fs.readFileSync(path.join(dir, 'common.cpp'), 'utf8')
  assert.match(src, /Content-Security-Policy", "frame-ancestors 'none';"/); assert.doesNotMatch(src, /connect-src/)
})
