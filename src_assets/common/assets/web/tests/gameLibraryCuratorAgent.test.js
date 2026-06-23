import test from 'node:test'
import assert from 'node:assert/strict'

import {
  createGameLibraryCuratorAgent,
  GAME_LIBRARY_SKILL_IDS,
  getGameResourceReviewReasons,
  needsGameResourceReview,
} from '../utils/agents/gameLibrary/gameLibraryCuratorAgent.js'
import { createCoverSelectionSkill } from '../utils/agents/gameLibrary/skills/coverSelectionSkill.js'
import { createGameTitleNormalizeSkill } from '../utils/agents/gameLibrary/skills/gameTitleNormalizeSkill.js'
import { createScanOverrideMemorySkill } from '../utils/agents/gameLibrary/skills/scanOverrideMemorySkill.js'

test('game library curator agent runs memory, title, and cover skills in order', async () => {
  const calls = []
  const agent = createGameLibraryCuratorAgent({
    skills: [
      createScanOverrideMemorySkill({
        applyOverrides(apps) {
          calls.push('memory')
          return apps.map((app) => ({ ...app, name: 'Remembered Name' }))
        },
      }),
      createGameTitleNormalizeSkill({
        async enhanceNames(apps) {
          calls.push('title')
          return apps.map((app) => ({
            ...app,
            name: 'Canonical Game',
            'canonical-name': 'Canonical Game',
            'ai-confidence': 0.94,
          }))
        },
      }),
      createCoverSelectionSkill({
        async findCover() {
          calls.push('cover')
          return {
            saveUrl: 'cover.jpg',
            source: 'igdb',
            name: 'Canonical Game',
            searchTerm: 'Canonical Game',
            aiCoverConfidence: 0.9,
            aiCoverReason: 'Exact title match',
          }
        },
      }),
    ],
  })

  const result = await agent.run([
    { name: 'raw.exe', cmd: 'C:/Games/raw.exe', 'is-game': true },
  ])

  assert.deepEqual(calls, ['memory', 'title', 'cover'])
  assert.equal(result.apps[0].name, 'Canonical Game')
  assert.equal(result.apps[0]['image-path'], 'cover.jpg')
  assert.equal(result.stats.titleChanges, 1)
  assert.equal(result.stats.coversFound, 1)
  assert.equal(result.events.length, 3)
})

test('game library curator agent can run a selected skill subset', async () => {
  const calls = []
  const agent = createGameLibraryCuratorAgent({
    skills: [
      createScanOverrideMemorySkill({
        applyOverrides(apps) {
          calls.push('memory')
          return apps
        },
      }),
      createGameTitleNormalizeSkill({
        async enhanceNames(apps) {
          calls.push('title')
          return apps
        },
      }),
    ],
  })

  await agent.run([{ name: 'raw.exe' }], {
    enabledSkills: [GAME_LIBRARY_SKILL_IDS.titleNormalize],
  })

  assert.deepEqual(calls, ['title'])
})

test('game resource review policy flags low confidence and missing cover', () => {
  const app = {
    name: 'Maybe Game',
    'is-game': true,
    'canonical-name': '',
    'ai-confidence': 0.5,
  }

  assert.equal(needsGameResourceReview(app), true)
  assert.deepEqual(getGameResourceReviewReasons(app, { locale: 'en' }), [
    'Low name confidence 50%',
    'Missing canonical name',
    'Missing cover',
  ])
  assert.deepEqual(getGameResourceReviewReasons(app, { locale: 'zh-CN' }), [
    '\u540d\u79f0\u7f6e\u4fe1\u5ea6 50%',
    '\u7f3a\u5c11\u89c4\u8303\u540d\u79f0',
    '\u7f3a\u5c11\u5c01\u9762',
  ])
})
