import test from 'node:test'
import assert from 'node:assert/strict'

import {
  createGameLibraryCuratorAgent,
  createGameLibrarySkill,
  GAME_LIBRARY_AGENT_CAPABILITIES,
  GAME_LIBRARY_SKILL_IDS,
  getDefaultEnabledGameLibrarySkillIds,
  getGameLibraryCapabilities,
  getGameLibraryCapabilityIcon,
  getGameLibraryCapabilityLabel,
  getGameLibrarySelectableCapabilities,
  getGameResourceReviewReasons,
  normalizeGameLibrarySkillIds,
  needsGameResourceReview,
  registerGameLibrarySkillExtension,
} from '../utils/agents/gameLibrary/gameLibraryCuratorAgent.js'
import { createCoverSelectionSkill } from '../utils/agents/gameLibrary/skills/coverSelectionSkill.js'
import { getGameResourceKey } from '../utils/agents/gameLibrary/skills/coverSelectionSkill.js'
import { createGameTitleNormalizeSkill } from '../utils/agents/gameLibrary/skills/gameTitleNormalizeSkill.js'
import { createScanOverrideMemorySkill } from '../utils/agents/gameLibrary/skills/scanOverrideMemorySkill.js'

test('createGameLibrarySkill normalizes extension skill definitions', async () => {
  const skill = createGameLibrarySkill({
    id: ' game.test.skill ',
    async run(context) {
      return context
    },
  })

  assert.equal(skill.id, 'game.test.skill')
  assert.equal(skill.type, 'extension')
  assert.equal(skill.label, 'game.test.skill')

  await assert.doesNotReject(() => skill.run({ apps: [] }))
})

test('createGameLibrarySkill rejects invalid skill definitions', () => {
  assert.throws(() => createGameLibrarySkill({ run() {} }), /non-empty id/)
  assert.throws(() => createGameLibrarySkill({ id: 'game.test.invalid' }), /run\(context\)/)
})

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

test('cover selection skill limits concurrent cover lookups', async () => {
  let active = 0
  let maxActive = 0
  const agent = createGameLibraryCuratorAgent({
    skills: [
      createCoverSelectionSkill({
        concurrency: 2,
        async findCover(app) {
          active += 1
          maxActive = Math.max(maxActive, active)
          await new Promise((resolve) => setTimeout(resolve, 5))
          active -= 1
          return {
            saveUrl: `${app.name}.jpg`,
            source: 'steam',
            name: app.name,
          }
        },
      }),
    ],
  })

  const result = await agent.run(Array.from({ length: 5 }, (_, index) => ({ name: `Game ${index}` })))

  assert.equal(maxActive, 2)
  assert.equal(result.stats.coversFound, 5)
  assert.deepEqual(result.apps.map((app) => app['image-path']), [
    'Game 0.jpg',
    'Game 1.jpg',
    'Game 2.jpg',
    'Game 3.jpg',
    'Game 4.jpg',
  ])
})

test('game library curator agent continues after a skill failure', async () => {
  const errors = []
  const calls = []
  const agent = createGameLibraryCuratorAgent({
    skills: [
      createGameTitleNormalizeSkill({
        async enhanceNames() {
          calls.push('title')
          throw new Error('metadata unavailable')
        },
      }),
      createCoverSelectionSkill({
        async findCover() {
          calls.push('cover')
          return { saveUrl: 'fallback-cover.jpg', source: 'steam', name: 'Raw Game' }
        },
      }),
    ],
  })

  const result = await agent.run([{ name: 'Raw Game', cmd: 'raw.exe' }], {
    onSkillError(skillId, error) {
      errors.push([skillId, error.message])
    },
  })

  assert.deepEqual(calls, ['title', 'cover'])
  assert.deepEqual(errors, [[GAME_LIBRARY_SKILL_IDS.titleNormalize, 'metadata unavailable']])
  assert.equal(result.stats.skillFailures, 1)
  assert.equal(result.apps[0]['image-path'], 'fallback-cover.jpg')
})

test('game resource keys stay unique for duplicate launch metadata', () => {
  const first = { source_path: 'C:/Games/Same', cmd: 'same.exe' }
  const second = { source_path: 'C:/Games/Same', cmd: 'same.exe' }

  assert.notEqual(getGameResourceKey(first, 0), getGameResourceKey(second, 1))
  assert.equal(getGameResourceKey({ ...first, '__scan-key': 'scan-1-0' }, 7), 'scan-1-0')
})

test('game library curator capabilities expose user-selectable skills', () => {
  const selectableSkillIds = getGameLibrarySelectableCapabilities()
    .map((capability) => capability.skillId)

  assert.deepEqual(selectableSkillIds, [
    GAME_LIBRARY_SKILL_IDS.titleNormalize,
    GAME_LIBRARY_SKILL_IDS.coverSelection,
  ])
  assert.equal(
    GAME_LIBRARY_AGENT_CAPABILITIES.find((capability) => capability.skillId === GAME_LIBRARY_SKILL_IDS.scanOverrideMemory)
      .userSelectable,
    false
  )
  assert.equal(getGameLibraryCapabilityIcon(GAME_LIBRARY_SKILL_IDS.titleNormalize), 'fa-wand-magic-sparkles')
  assert.equal(getGameLibraryCapabilityLabel(GAME_LIBRARY_SKILL_IDS.coverSelection), 'AI cover matching')
  assert.equal(getGameLibraryCapabilityLabel(GAME_LIBRARY_SKILL_IDS.coverSelection, { locale: 'zh-CN' }), 'AI \u5c01\u9762\u5339\u914d')
})

test('game library curator skill id helpers keep required skills enabled', () => {
  assert.deepEqual(getDefaultEnabledGameLibrarySkillIds(), [
    GAME_LIBRARY_SKILL_IDS.scanOverrideMemory,
    GAME_LIBRARY_SKILL_IDS.titleNormalize,
    GAME_LIBRARY_SKILL_IDS.coverSelection,
  ])
  assert.deepEqual(normalizeGameLibrarySkillIds([GAME_LIBRARY_SKILL_IDS.coverSelection, 'unknown.skill']), [
    GAME_LIBRARY_SKILL_IDS.scanOverrideMemory,
    GAME_LIBRARY_SKILL_IDS.coverSelection,
  ])
})

test('game library curator supports registering extension skills', async () => {
  const calls = []
  const unregister = registerGameLibrarySkillExtension({
    skill: {
      id: 'game.test.annotate',
      type: 'metadata',
      label: 'Test annotation',
      async run(context) {
        calls.push('extension')
        return {
          ...context,
          apps: context.apps.map((app) => ({ ...app, 'test-extension': true })),
        }
      },
    },
    capability: {
      icon: 'fa-vial',
      defaultEnabled: false,
      labels: { zh: '\u6d4b\u8bd5\u6807\u6ce8' },
    },
  })

  try {
    assert.equal(getGameLibraryCapabilityIcon('game.test.annotate'), 'fa-vial')
    assert.equal(getGameLibraryCapabilityLabel('game.test.annotate', { locale: 'zh-CN' }), '\u6d4b\u8bd5\u6807\u6ce8')
    assert.ok(getGameLibrarySelectableCapabilities().some((capability) => capability.skillId === 'game.test.annotate'))
    assert.ok(!getDefaultEnabledGameLibrarySkillIds().includes('game.test.annotate'))

    const result = await createGameLibraryCuratorAgent().run([{ name: 'Raw Game' }], {
      enabledSkills: ['game.test.annotate'],
    })

    assert.deepEqual(calls, ['extension'])
    assert.equal(result.apps[0]['test-extension'], true)
  } finally {
    unregister()
  }

  assert.ok(!getGameLibraryCapabilities().some((capability) => capability.skillId === 'game.test.annotate'))
})

test('game library curator rejects duplicate extension skills', () => {
  assert.throws(
    () => registerGameLibrarySkillExtension({
      skill: {
        id: GAME_LIBRARY_SKILL_IDS.coverSelection,
        async run(context) {
          return context
        },
      },
    }),
    /already registered/
  )
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

test('game resource review policy ignores blank numeric confidence fields', () => {
  const app = {
    name: 'Known Game',
    'is-game': true,
    'canonical-name': 'Known Game',
    'image-path': 'known.jpg',
    'ai-confidence': '',
    'ai-cover-confidence': '',
    'cover-match-confidence': '',
  }

  assert.deepEqual(getGameResourceReviewReasons(app, { locale: 'en' }), [])
  assert.equal(needsGameResourceReview(app), false)
})
