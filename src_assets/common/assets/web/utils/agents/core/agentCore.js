export function createAgentSkill(definition = {}, options = {}) {
  const id = typeof definition.id === 'string' ? definition.id.trim() : ''
  const skillSubject = options.skillSubject || 'Agent skills'
  const runSubject = options.runSubject || 'Agent skill'

  if (!id) {
    throw new Error(`${skillSubject} require a non-empty id`)
  }
  if (typeof definition.run !== 'function') {
    throw new Error(`${runSubject} requires run(context): ${id}`)
  }

  return {
    ...definition,
    id,
    type: definition.type || options.defaultType || 'extension',
    label: definition.label || id,
  }
}

export function createCapabilityFromSkill(skill, capability = {}) {
  return {
    stage: capability.stage || skill.type || 'extension',
    icon: capability.icon || 'fa-bolt',
    label: capability.label || skill.label || skill.id,
    labels: capability.labels || {},
    required: capability.required === true,
    defaultEnabled: capability.defaultEnabled === true,
    userSelectable: capability.userSelectable !== false,
    ...capability,
    skillId: skill.id,
  }
}

export function createSkillRegistry(options = {}) {
  const baseCapabilities = options.baseCapabilities || []
  const createSkill = options.createSkill || createAgentSkill
  const createCapability = options.createCapability || createCapabilityFromSkill
  const extensions = []

  function getCapabilities() {
    return [
      ...baseCapabilities,
      ...extensions.map((extension) => extension.capability),
    ]
  }

  function hasCapability(skillId, capabilities = getCapabilities()) {
    return capabilities.some((capability) => capability.skillId === skillId)
  }

  function registerExtension(extension = {}) {
    const skill = createSkill(extension.skill)

    if (hasCapability(skill.id)) {
      throw new Error(`${options.duplicateMessage || 'Agent skill already registered'}: ${skill.id}`)
    }

    const entry = {
      skill,
      capability: createCapability(skill, extension.capability),
    }
    extensions.push(entry)

    return () => {
      const index = extensions.indexOf(entry)
      if (index !== -1) {
        extensions.splice(index, 1)
      }
    }
  }

  function getExtensionSkills() {
    return extensions.map((extension) => extension.skill)
  }

  return {
    getCapabilities,
    getExtensionSkills,
    hasCapability,
    registerExtension,
  }
}

export function getAgentCapability(skillId, capabilities = []) {
  return capabilities.find((capability) => capability.skillId === skillId) || null
}

export function getSelectableAgentCapabilities(capabilities = []) {
  return capabilities.filter((capability) => capability.userSelectable)
}

export function getDefaultEnabledSkillIds(capabilities = []) {
  return capabilities
    .filter((capability) => capability.defaultEnabled || capability.required)
    .map((capability) => capability.skillId)
}

export function normalizeEnabledSkillIds(skillIds, capabilities = []) {
  const known = new Set(capabilities.map((capability) => capability.skillId))
  const enabled = Array.isArray(skillIds) ? skillIds.filter((skillId) => known.has(skillId)) : []
  const required = capabilities
    .filter((capability) => capability.required)
    .map((capability) => capability.skillId)

  return Array.from(new Set([...required, ...enabled]))
}

export function getAgentCapabilityIcon(skillId, capabilities = []) {
  return getAgentCapability(skillId, capabilities)?.icon || 'fa-bolt'
}

export function getAgentCapabilityLabel(skillId, options = {}) {
  const capability = getAgentCapability(skillId, options.capabilities || [])
  if (!capability) return skillId

  const locale = String(options.locale || '').toLowerCase()
  if (locale.startsWith('zh')) {
    return capability.labels?.zh || capability.label
  }

  return capability.label || skillId
}

function filterAgentSkills(skills, enabledSkills) {
  if (!Array.isArray(enabledSkills) || enabledSkills.length === 0) {
    return skills
  }

  const enabled = new Set(enabledSkills)
  return skills.filter((skill) => enabled.has(skill.id))
}

function normalizeAgentContext(context, input, runOptions) {
  const normalized = {
    input,
    events: [],
    stats: {},
    options: runOptions,
    ...(context && typeof context === 'object' ? context : {}),
  }

  normalized.events = Array.isArray(normalized.events) ? normalized.events : []
  normalized.stats = normalized.stats && typeof normalized.stats === 'object' ? normalized.stats : {}
  normalized.options = normalized.options && typeof normalized.options === 'object' ? normalized.options : runOptions

  return normalized
}

export function createAgent(options = {}) {
  const id = options.id || 'agent'
  const skills = options.skills || []
  const createContext = options.createContext || ((input, runOptions) => ({
    input,
    events: [],
    stats: {},
    options: runOptions,
  }))

  return {
    id,
    skills,

    getSkill(skillId) {
      return skills.find((skill) => skill.id === skillId)
    },

    async run(input, runOptions = {}) {
      let context = normalizeAgentContext(createContext(input, runOptions), input, runOptions)

      for (const skill of filterAgentSkills(skills, runOptions.enabledSkills)) {
        try {
          const nextContext = await skill.run(context)
          if (!nextContext || typeof nextContext !== 'object') {
            throw new Error(`Agent skill returned an invalid context: ${skill.id}`)
          }
          context = normalizeAgentContext(nextContext, input, runOptions)
        } catch (error) {
          context.options?.onSkillError?.(skill.id, error)
          context.events?.push({
            skillId: skill.id,
            type: 'skill:error',
            error,
          })
          context.stats = {
            ...(context.stats || {}),
            skillFailures: (context.stats?.skillFailures || 0) + 1,
          }
          if (runOptions.stopOnSkillError) {
            throw error
          }
        }
      }

      return context
    },
  }
}
