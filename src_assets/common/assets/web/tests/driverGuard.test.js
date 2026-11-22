import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

import { driverGuardTextsEn, driverGuardTextsZh } from 'foundation-ui/driverGuard'

const here = path.dirname(fileURLToPath(import.meta.url))

test('the home page shows the notice and the tray text matches it word for word', () => {
  const home = fs.readFileSync(path.join(here, '../views/Home.vue'), 'utf8')
  assert.match(home, /<MDriverGuardNotice[^>]*:guard="driverGuard"/)
  // the host says the same sentences in the tray: every line of src/gpu_driver_text.h is a line of the foundation-ui texts
  const header = fs.readFileSync(path.join(here, '../../../../../src/gpu_driver_text.h'), 'utf8')
  const lines = Object.fromEntries([...header.matchAll(/constexpr const char \*(ZH|EN)_([A-Z_]+) = "([^"]*)";/g)].map((m) => [`${m[1]}_${m[2]}`, m[3]]))
  const expected = (t, p) => ({
    [`${p}_PHASE_STARTUP`]: t.phases.startup, [`${p}_PHASE_PROBE`]: t.phases.probe, [`${p}_PHASE_IDLE`]: t.phases.idle, [`${p}_PHASE_STREAMING`]: t.phases.streaming,
    [`${p}_OUTCOME_CRASH`]: t.outcomes.crash, [`${p}_OUTCOME_HANG`]: t.outcomes.hang,
    [`${p}_DRIVER_TITLE`]: t.driverTitle, [`${p}_DRIVER_BODY`]: t.driverBody, [`${p}_DRIVER_BODY_NO_VERSION`]: t.driverBodyNoVersion,
    [`${p}_SAFE_TITLE`]: t.safeTitle, [`${p}_SAFE_BODY`]: t.safeBody,
  })
  const all = { ...expected(driverGuardTextsZh, 'ZH'), ...expected(driverGuardTextsEn, 'EN') }
  assert.equal(Object.keys(lines).length, Object.keys(all).length)
  for (const [name, text] of Object.entries(all)) assert.equal(lines[name], text, name)
})
