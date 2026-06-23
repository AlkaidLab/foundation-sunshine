import test from 'node:test'
import assert from 'node:assert/strict'

import {
  createAgent,
  createAgentSkill,
  createSkillRegistry,
  getAgentCapabilityIcon,
  getAgentCapabilityLabel,
  getDefaultEnabledSkillIds,
  getSelectableAgentCapabilities,
  normalizeEnabledSkillIds,
} from '../utils/agents/core/agentCore.js'

test('createAgentSkill normalizes reusable skill definitions', async () => {
  const skill = createAgentSkill({
    id: ' demo.skill ',
    async run(context) {
      return context
    },
  })

  assert.equal(skill.id, 'demo.skill')
  assert.equal(skill.type, 'extension')
  assert.equal(skill.label, 'demo.skill')
  await assert.doesNotReject(() => skill.run({ input: [] }))
})

test('createSkillRegistry registers extension capabilities and supports unregister', () => {
  const registry = createSkillRegistry({
    baseCapabilities: [
      {
        skillId: 'demo.required',
        label: 'Required',
        required: true,
        defaultEnabled: true,
        userSelectable: false,
      },
    ],
  })

  const unregister = registry.registerExtension({
    skill: {
      id: 'demo.optional',
      type: 'metadata',
      label: 'Optional',
      async run(context) {
        return context
      },
    },
    capability: {
      icon: 'fa-vial',
      labels: { zh: '可选能力' },
      defaultEnabled: false,
      userSelectable: true,
    },
  })

  const capabilities = registry.getCapabilities()
  assert.deepEqual(getDefaultEnabledSkillIds(capabilities), ['demo.required'])
  assert.deepEqual(normalizeEnabledSkillIds(['demo.optional', 'missing'], capabilities), [
    'demo.required',
    'demo.optional',
  ])
  assert.deepEqual(getSelectableAgentCapabilities(capabilities).map((capability) => capability.skillId), [
    'demo.optional',
  ])
  assert.equal(getAgentCapabilityIcon('demo.optional', capabilities), 'fa-vial')
  assert.equal(getAgentCapabilityLabel('demo.optional', { locale: 'zh-CN', capabilities }), '可选能力')

  unregister()
  assert.ok(!registry.getCapabilities().some((capability) => capability.skillId === 'demo.optional'))
})

test('createAgent runs selected skills and continues after failures', async () => {
  const calls = []
  const errors = []
  const agent = createAgent({
    id: 'demo-agent',
    skills: [
      createAgentSkill({
        id: 'demo.fail',
        async run() {
          calls.push('fail')
          throw new Error('temporary failure')
        },
      }),
      createAgentSkill({
        id: 'demo.pass',
        async run(context) {
          calls.push('pass')
          return {
            ...context,
            input: [...context.input, 'done'],
          }
        },
      }),
    ],
  })

  const result = await agent.run([], {
    onSkillError(skillId, error) {
      errors.push([skillId, error.message])
    },
  })

  assert.equal(agent.id, 'demo-agent')
  assert.deepEqual(calls, ['fail', 'pass'])
  assert.deepEqual(errors, [['demo.fail', 'temporary failure']])
  assert.deepEqual(result.input, ['done'])
  assert.equal(result.stats.skillFailures, 1)
  assert.equal(result.events[0].type, 'skill:error')
})

test('createAgent treats invalid skill context as a recoverable skill failure', async () => {
  const errors = []
  const calls = []
  const agent = createAgent({
    skills: [
      createAgentSkill({
        id: 'demo.invalid',
        async run() {
          calls.push('invalid')
          return null
        },
      }),
      createAgentSkill({
        id: 'demo.recover',
        async run(context) {
          calls.push('recover')
          context.events.push({ skillId: 'demo.recover', type: 'skill:recovered' })
          return {
            ...context,
            input: [...context.input, 'recovered'],
          }
        },
      }),
    ],
  })

  const result = await agent.run([], {
    onSkillError(skillId, error) {
      errors.push([skillId, error.message])
    },
  })

  assert.deepEqual(calls, ['invalid', 'recover'])
  assert.equal(result.stats.skillFailures, 1)
  assert.deepEqual(result.input, ['recovered'])
  assert.equal(result.events[0].type, 'skill:error')
  assert.equal(result.events[1].type, 'skill:recovered')
  assert.deepEqual(errors, [['demo.invalid', 'Agent skill returned an invalid context: demo.invalid']])
})
