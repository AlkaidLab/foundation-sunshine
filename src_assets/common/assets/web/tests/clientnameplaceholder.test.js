import { test } from "node:test"
import assert from "node:assert/strict"
import fs from "node:fs"

// (sessionClientLabel itself is covered by foundation-ui test/clientlabel.test.js.) The host names a session client "Named Zako" when the client sent no name and was paired without one: the audio chain and the diagnostics say what the host derived (the client family), never the placeholder.
const read = (f) => fs.readFileSync(new URL(`../${f}`, import.meta.url), "utf8")
test("the audio chain and the diagnostics use that label", () => {
  assert.match(read("configs/tabs/audiovideo/AvRoutes.vue"), /title: sessionClientLabel\(props\.session\)/)
  assert.match(read("composables/useDiagnostics.js"), /sessionClientLabel\(s\) \? \{ label: .Client./)
  assert.doesNotMatch(read("configs/tabs/audiovideo/AvRoutes.vue"), /props\.session\?\.client_name/)
})

test("Web Home reads the session through realSession and gives the star banner the family of the session", () => {
  const home = read("views/Home.vue")
  assert.match(home, /const rawSession = computed\(\(\) => realSession\(live\.sessions\.value\[0\] \|\| null, shellLive\.clients\.value\?\.list\)\)/)
  assert.match(home, /family: session\.value\.client_family \|\| '', os: session\.value\.client_os \|\| '', official: session\.value\.client_official/)
})

import { pickPerf } from "../utils/homeData.js"
test("Home reads the measured bitrate from the session or the perf snapshot and keeps the budget apart (mock data: the real host does not send it yet)", () => {
  const perf = pickPerf({ sessions: [{ active: true, session_id: 2, bitrate_kbps: 113000, measured_bitrate_kbps: 96400, fps: 120, host_latency: {} }] }, 2)
  assert.equal(perf.bitrateKbps, 113000); assert.equal(perf.measuredKbps, 96400)
  assert.equal(pickPerf({ sessions: [{ active: true, session_id: 2, bitrate_kbps: 113000, fps: 120, host_latency: {} }] }, 2).measuredKbps, null)
  const home = read("views/Home.vue")
  assert.match(home, /measured_bitrate_kbps: m/); assert.match(home, /bitrateSample\(session\.value\)/); assert.match(home, /history\.sample\('bitrate', bitrateSample\(s\)\)/)
})
