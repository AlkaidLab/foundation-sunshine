import test from 'node:test'
import assert from 'node:assert/strict'

import {
  createLibraryMaintenanceAgent,
  createLibraryMaintenanceSkill,
  getDefaultEnabledLibraryMaintenanceSkillIds,
  getLibraryMaintenanceCapabilityIcon,
  getLibraryMaintenanceCapabilityLabel,
  getLibraryMaintenanceSelectableCapabilities,
  LIBRARY_MAINTENANCE_AGENT_ID,
  LIBRARY_MAINTENANCE_SKILL_IDS,
  normalizeLibraryMaintenanceSkillIds,
  registerLibraryMaintenanceSkillExtension,
  runLibraryMaintenanceAgent,
} from '../utils/agents/libraryMaintenance/libraryMaintenanceAgent.js'

test('library maintenance agent reports duplicate launch targets and entry issues', async () => {
  const result = await runLibraryMaintenanceAgent([
    {
      name: 'Game One',
      cmd: 'C:\\Games\\Same\\game.exe',
      'working-dir': 'C:\\Games\\Same',
      'image-path': 'cover.png',
    },
    {
      name: 'Game One Copy',
      cmd: 'c:/games/same/game.exe',
      'working-dir': 'c:/games/same',
      'image-path': 'copy.png',
    },
    {
      name: '',
      cmd: '',
      'image-path': '',
    },
  ])

  assert.equal(result.stats.duplicateLaunchTargets, 1)
  assert.equal(result.stats.entryHealthIssues, 3)
  assert.equal(result.issues.length, 4)
  assert.deepEqual(
    result.issues.map((issue) => issue.type),
    ['duplicate-launch-target', 'missing-name', 'missing-command', 'missing-cover']
  )
  assert.deepEqual(result.issues[0].appIndexes, [0, 1])
  assert.equal(result.events.length, 2)
})

test('library maintenance agent can run a selected skill subset', async () => {
  const agent = createLibraryMaintenanceAgent()
  const result = await agent.run([
    { name: 'No Cover', cmd: 'game.exe', 'image-path': '' },
  ], {
    enabledSkills: [LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth],
  })

  assert.equal(agent.id, LIBRARY_MAINTENANCE_AGENT_ID)
  assert.equal(result.stats.duplicateLaunchTargets, undefined)
  assert.equal(result.stats.entryHealthIssues, 1)
  assert.deepEqual(result.issues.map((issue) => issue.type), ['missing-cover'])
})

test('library maintenance capabilities expose selectable skills', () => {
  assert.deepEqual(getDefaultEnabledLibraryMaintenanceSkillIds(), [
    LIBRARY_MAINTENANCE_SKILL_IDS.duplicateLaunchTargets,
    LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth,
  ])
  assert.deepEqual(
    normalizeLibraryMaintenanceSkillIds([LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth, 'unknown.skill']),
    [LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth]
  )
  assert.deepEqual(
    getLibraryMaintenanceSelectableCapabilities().map((capability) => capability.skillId),
    [
      LIBRARY_MAINTENANCE_SKILL_IDS.duplicateLaunchTargets,
      LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth,
    ]
  )
  assert.equal(getLibraryMaintenanceCapabilityIcon(LIBRARY_MAINTENANCE_SKILL_IDS.duplicateLaunchTargets), 'fa-copy')
  assert.equal(getLibraryMaintenanceCapabilityLabel(LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth), 'Entry health checks')
  assert.equal(
    getLibraryMaintenanceCapabilityLabel(LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth, { locale: 'zh-CN' }),
    '条目健康检查'
  )
})

test('library maintenance supports extension skills', async () => {
  const unregister = registerLibraryMaintenanceSkillExtension({
    skill: createLibraryMaintenanceSkill({
      id: 'library.test.annotate',
      type: 'quality',
      async run(context) {
        return {
          ...context,
          issues: [
            ...(context.issues || []),
            {
              id: 'test:issue',
              type: 'test-issue',
              severity: 'info',
              skillId: 'library.test.annotate',
              appIndexes: [],
              message: 'Test issue',
            },
          ],
        }
      },
    }),
    capability: {
      icon: 'fa-vial',
      labels: { zh: '测试维护能力' },
      defaultEnabled: false,
      userSelectable: true,
    },
  })

  try {
    assert.equal(getLibraryMaintenanceCapabilityIcon('library.test.annotate'), 'fa-vial')
    assert.equal(getLibraryMaintenanceCapabilityLabel('library.test.annotate', { locale: 'zh-CN' }), '测试维护能力')

    const result = await createLibraryMaintenanceAgent().run([], {
      enabledSkills: ['library.test.annotate'],
    })

    assert.deepEqual(result.issues.map((issue) => issue.type), ['test-issue'])
  } finally {
    unregister()
  }
})

test('library maintenance rejects duplicate extension skills', () => {
  assert.throws(
    () => registerLibraryMaintenanceSkillExtension({
      skill: {
        id: LIBRARY_MAINTENANCE_SKILL_IDS.entryHealth,
        async run(context) {
          return context
        },
      },
    }),
    /already registered/
  )
})
