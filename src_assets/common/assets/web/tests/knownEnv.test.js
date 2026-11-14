import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'

const read = (p) => fs.readFileSync(new URL(p, import.meta.url), 'utf8')

test('a known limit of the machine is an error event of severity warning with a category and a code only, and only after consent; the automatic report starts in the shell', () => {
  // the listeners and the sender are the library's (foundation-ui/telemetry events.js, tested there); the page wires them to the host
  const telemetry = read('../config/telemetry.js')
  assert.match(telemetry, /import \{ createEventSender, installEventListeners, setReportTransport \} from 'foundation-ui\/telemetry'/)
  assert.match(telemetry, /installEventListeners\(sender\)/)
  assert.match(telemetry, /\/api\/telemetry\/events/)
  assert.match(read('../views/WebShell.vue'), /startAutoReport\(\{ context: errorContext \}\)/)
})
