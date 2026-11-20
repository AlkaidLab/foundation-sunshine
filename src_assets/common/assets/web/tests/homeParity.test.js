import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { pickPerf } from '../utils/homeData.js'

const home = fs.readFileSync(new URL('../views/Home.vue', import.meta.url), 'utf8')

// the overview of the Web UI and of the control panel are one planner over one catalogue: what differs is only the data each product hands in
test('the perf session keeps the stages of the host pipeline, so the frame-budget tile has its split here as in the control panel', () => {
  const stages = { convert: { samples: 4, avg_ms: 0.8 }, total: { samples: 4, avg_ms: 5.5 } }
  assert.deepEqual(pickPerf({ sessions: [{ session_id: 1, active: true, pipeline: stages, host_latency: { avg_ms: 5 } }] }).pipeline, stages)
  assert.equal(pickPerf({ sessions: [{ session_id: 1, active: true, host_latency: { avg_ms: 5 } }] }).pipeline, null)
  // only the stages of the session that is shown: after a switch of session the perf of the old one may still be there
  assert.match(home, /\.\.\.pipelineAverages\(\{ pipeline: perf\.value\?\.sessionId === s\.session_id \? perf\.value\?\.pipeline : null \}\)/)
  assert.match(home, /createSerialCalls, pipelineAverages \} from 'foundation-ui\/vue'/)
})

test("the host audio is a tile of the overview, not a section below it: the tile's switch sends the command, a refusal puts the switch back", () => {
  assert.ok(!/home-live-controls/.test(home), 'no ad-hoc section under the tiles')
  assert.ok(!/<MSection v-if="session"/.test(home))
  assert.match(home, /case 'runtime-host-audio': setHostAudio\(a\.value\); break/)
  assert.match(home, /apiPostJson\('\/api\/runtime\/audio', \{ clientname: client, enabled \}\)/)
  // one command per client at a time, in the order chosen: the last choice wins
  assert.match(home, /const setHostAudio = createQueuedCalls\(async \(\{ client, enabled, failed \}\) => \{/)
  assert.match(home, /\}, \(a\) => a\.client\)\n/)
  assert.match(home, /catch \{ failed\?\.\(\); say\(\$tp\('config\.host_audio_failed'/)
})

// the queue the host audio uses (foundation-ui): the first choice goes out, a choice made while it runs waits, only the newest waits, and it is sent after the first has settled
test('host audio commands of one client run one at a time and the last choice wins', async () => {
  const { createQueuedCalls } = await import(new URL('../../../../../node_modules/foundation-ui/src/vue/queuedCalls.js', import.meta.url))
  const sent = []
  const gates = []
  const send = createQueuedCalls((a) => new Promise((resolve) => { sent.push(a.enabled); gates.push(resolve) }), (a) => a.client)
  send({ client: 'Pixel 9', enabled: true })
  send({ client: 'Pixel 9', enabled: false })
  send({ client: 'Pixel 9', enabled: true })
  send({ client: 'iPad', enabled: true })
  assert.deepEqual(sent, [true, true], 'the second and third wait; another client does not wait for this one')
  gates[0]()
  await new Promise((r) => setTimeout(r, 0))
  assert.deepEqual(sent, [true, true, true], 'after the first settles only the newest choice goes out (the one in the middle is dropped)')
})
