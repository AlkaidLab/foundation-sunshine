import assert from 'node:assert/strict'
import test from 'node:test'

import { getTrueHdrSetupHint } from '../utils/trueHdrSetupHint.js'

const degradedPipeline = (reason) => ({
  hdr_mode: 'synthetic_hdr',
  synthetic_hdr_state: 'degraded',
  synthetic_hdr_failure_reason: reason,
})

test('load failure gives deterministic build and colocated-runtime guidance', () => {
  const hint = getTrueHdrSetupHint(degradedPipeline('backend_load_failed:126'))

  assert.match(hint, /build-rtx-hdr-backend\.ps1/)
  assert.match(hint, /user-provided nvngx_truehdr\.dll beside it/)
  assert.match(hint, /set the backend DLL's absolute path/)
})

test('invalid backend path uses the same actionable setup guidance', () => {
  const hint = getTrueHdrSetupHint(degradedPipeline('backend_path_not_absolute'))

  assert.match(hint, /build-rtx-hdr-backend\.ps1/)
})

test('runtime backend failures do not recommend rebuilding the backend', () => {
  const hint = getTrueHdrSetupHint(degradedPipeline('backend_create_runtime_unavailable'))

  assert.match(hint, /GPU\/driver\/SDK issue/)
  assert.match(hint, /restart the stream/)
  assert.doesNotMatch(hint, /build-rtx-hdr-backend/)
})

test('healthy, SDR, and unrelated degraded pipelines have no TrueHDR setup hint', () => {
  assert.equal(getTrueHdrSetupHint(null), '')
  assert.equal(
    getTrueHdrSetupHint({ ...degradedPipeline('backend_load_failed:126'), hdr_mode: 'sdr' }),
    '',
  )
  assert.equal(
    getTrueHdrSetupHint({ ...degradedPipeline('backend_load_failed:126'), synthetic_hdr_state: 'active' }),
    '',
  )
  assert.equal(getTrueHdrSetupHint(degradedPipeline('format_unsupported')), '')
})
