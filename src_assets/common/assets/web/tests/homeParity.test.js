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
  assert.match(home, /\.\.\.pipelineAverages\(\{ pipeline: perf\.value\?\.pipeline \}\)/)
  assert.match(home, /createSerialCalls, pipelineAverages \} from 'foundation-ui\/vue'/)
})

test("the host audio is a tile of the overview, not a section below it: the tile's switch sends the command, a refusal puts the switch back", () => {
  assert.ok(!/home-live-controls/.test(home), 'no ad-hoc section under the tiles')
  assert.ok(!/<MSection v-if="session"/.test(home))
  assert.match(home, /case 'runtime-host-audio': setHostAudio\(a\.value\); break/)
  assert.match(home, /apiPostJson\('\/api\/runtime\/audio', \{ clientname: client, enabled \}\)/)
  assert.match(home, /catch \{ failed\?\.\(\); say\(\$tp\('config\.host_audio_failed'/)
})
