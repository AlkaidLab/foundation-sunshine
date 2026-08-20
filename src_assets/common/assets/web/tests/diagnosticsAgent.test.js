import test from 'node:test'
import assert from 'node:assert/strict'

import {
  createDiagnosticsAgent,
  createDiagnosticsSkill,
  DIAGNOSTICS_AGENT_ID,
  DIAGNOSTICS_SKILL_IDS,
  getDefaultEnabledDiagnosticsSkillIds,
  getDiagnosticsCapabilityIcon,
  getDiagnosticsCapabilityLabel,
  getDiagnosticsSelectableCapabilities,
  normalizeDiagnosticsSkillIds,
  registerDiagnosticsSkillExtension,
  runDiagnosticsAgent,
} from '../utils/agents/diagnostics/diagnosticsAgent.js'
import { findLogPatternFindings } from '../utils/agents/diagnostics/skills/logPatternSkill.js'

test('diagnostics agent summarizes severity, detects patterns, and suggests fixes', async () => {
  const logs = [
    '[2026-06-23] Warning: configuration option deprecated',
    '[2026-06-23] Error: NVENC encoder failed to initialize',
    '[2026-06-23] Error: RTSP connection timed out',
    '[2026-06-23] Fatal: Sunshine cannot continue',
  ].join('\n')

  const result = await runDiagnosticsAgent(logs)

  assert.equal(result.stats.fatalLogLines, 1)
  assert.equal(result.stats.errorLogLines, 2)
  assert.equal(result.stats.warningLogLines, 1)
  assert.equal(result.stats.logPatternFindings, 3)
  assert.equal(result.stats.logRemediationSuggestions, 3)
  assert.deepEqual(
    result.findings.map((finding) => finding.type),
    ['config-risk', 'encoder-failure', 'network-timeout']
  )
  assert.deepEqual(
    result.suggestions.map((suggestion) => suggestion.findingType),
    ['config-risk', 'encoder-failure', 'network-timeout']
  )
  assert.ok(result.suggestions[1].actions.some((action) => action.includes('encoder')))
  assert.equal(result.events.length, 3)
})

test('diagnostics agent can run a selected skill subset', async () => {
  const agent = createDiagnosticsAgent()
  const result = await agent.run('Error: NVENC encoder failed', {
    enabledSkills: [DIAGNOSTICS_SKILL_IDS.logPatterns],
  })

  assert.equal(agent.id, DIAGNOSTICS_AGENT_ID)
  assert.equal(result.severitySummary, null)
  assert.equal(result.stats.logPatternFindings, 1)
})

test('log pattern detection only flags explicit port bind failures', () => {
  const informational = findLogPatternFindings('Info: Streaming server is using port 47990')
  assert.ok(!informational.some((finding) => finding.type === 'port-bind-failure'))

  const failure = findLogPatternFindings('Error: failed to bind port 47990 because address already in use')
  assert.ok(failure.some((finding) => finding.type === 'port-bind-failure'))
})

test('diagnostics capabilities expose selectable skills', () => {
  assert.deepEqual(getDefaultEnabledDiagnosticsSkillIds(), [
    DIAGNOSTICS_SKILL_IDS.logSeverity,
    DIAGNOSTICS_SKILL_IDS.logPatterns,
    DIAGNOSTICS_SKILL_IDS.logRemediation,
  ])
  assert.deepEqual(
    normalizeDiagnosticsSkillIds([DIAGNOSTICS_SKILL_IDS.logPatterns, 'unknown.skill']),
    [DIAGNOSTICS_SKILL_IDS.logPatterns]
  )
  assert.deepEqual(
    getDiagnosticsSelectableCapabilities().map((capability) => capability.skillId),
    [
      DIAGNOSTICS_SKILL_IDS.logSeverity,
      DIAGNOSTICS_SKILL_IDS.logPatterns,
      DIAGNOSTICS_SKILL_IDS.logRemediation,
    ]
  )
  assert.equal(getDiagnosticsCapabilityIcon(DIAGNOSTICS_SKILL_IDS.logPatterns), 'fa-magnifying-glass-chart')
  assert.equal(getDiagnosticsCapabilityIcon(DIAGNOSTICS_SKILL_IDS.logRemediation), 'fa-screwdriver-wrench')
  assert.equal(getDiagnosticsCapabilityLabel(DIAGNOSTICS_SKILL_IDS.logSeverity), 'Log severity summary')
  assert.equal(getDiagnosticsCapabilityLabel(DIAGNOSTICS_SKILL_IDS.logSeverity, { locale: 'zh-CN' }), '日志严重度摘要')
})

test('diagnostics supports extension skills', async () => {
  const unregister = registerDiagnosticsSkillExtension({
    skill: createDiagnosticsSkill({
      id: 'diagnostics.test.annotate',
      type: 'log-analysis',
      async run(context) {
        return {
          ...context,
          findings: [
            ...(context.findings || []),
            {
              id: 'test:finding',
              type: 'test-finding',
              severity: 'info',
              skillId: 'diagnostics.test.annotate',
              message: 'Test finding',
            },
          ],
        }
      },
    }),
    capability: {
      icon: 'fa-vial',
      labels: { zh: '测试诊断能力' },
      defaultEnabled: false,
      userSelectable: true,
    },
  })

  try {
    assert.equal(getDiagnosticsCapabilityIcon('diagnostics.test.annotate'), 'fa-vial')
    assert.equal(getDiagnosticsCapabilityLabel('diagnostics.test.annotate', { locale: 'zh-CN' }), '测试诊断能力')

    const result = await createDiagnosticsAgent().run('', {
      enabledSkills: ['diagnostics.test.annotate'],
    })

    assert.deepEqual(result.findings.map((finding) => finding.type), ['test-finding'])
  } finally {
    unregister()
  }
})

test('diagnostics rejects duplicate extension skills', () => {
  assert.throws(
    () => registerDiagnosticsSkillExtension({
      skill: {
        id: DIAGNOSTICS_SKILL_IDS.logSeverity,
        async run(context) {
          return context
        },
      },
    }),
    /already registered/
  )
})
