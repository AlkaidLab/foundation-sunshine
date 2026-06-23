import { createCoverSelectionSkill, COVER_SELECTION_SKILL_ID } from './skills/coverSelectionSkill.js'
import { createGameTitleNormalizeSkill, GAME_TITLE_NORMALIZE_SKILL_ID } from './skills/gameTitleNormalizeSkill.js'
import { createScanOverrideMemorySkill, SCAN_OVERRIDE_MEMORY_SKILL_ID } from './skills/scanOverrideMemorySkill.js'

export {
  applyCoverToGameResource,
  getGameResourceKey,
} from './skills/coverSelectionSkill.js'
export {
  getGameResourceReviewReasons,
  needsGameResourceReview,
  GAME_RESOURCE_REVIEW_THRESHOLDS,
} from './policies/reviewQueuePolicy.js'

export const GAME_LIBRARY_AGENT_ID = 'game-library-curator'

export const GAME_LIBRARY_SKILL_IDS = {
  scanOverrideMemory: SCAN_OVERRIDE_MEMORY_SKILL_ID,
  titleNormalize: GAME_TITLE_NORMALIZE_SKILL_ID,
  coverSelection: COVER_SELECTION_SKILL_ID,
}

export const GAME_LIBRARY_AGENT_CAPABILITIES = [
  {
    skillId: SCAN_OVERRIDE_MEMORY_SKILL_ID,
    stage: 'memory',
    icon: 'fa-clock-rotate-left',
    label: 'Confirmed overrides',
    labels: {
      zh: '\u5df2\u786e\u8ba4\u8986\u76d6',
    },
    required: true,
    defaultEnabled: true,
    userSelectable: false,
  },
  {
    skillId: GAME_TITLE_NORMALIZE_SKILL_ID,
    stage: 'metadata',
    icon: 'fa-wand-magic-sparkles',
    label: 'AI name cleanup',
    labels: {
      zh: 'AI \u540d\u79f0\u6e05\u6d17',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
  {
    skillId: COVER_SELECTION_SKILL_ID,
    stage: 'asset',
    icon: 'fa-image',
    label: 'AI cover matching',
    labels: {
      zh: 'AI \u5c01\u9762\u5339\u914d',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
]

const gameLibrarySkillExtensions = []

export function createGameLibrarySkill(definition = {}) {
  const id = typeof definition.id === 'string' ? definition.id.trim() : ''
  if (!id) {
    throw new Error('Game library skills require a non-empty id')
  }
  if (typeof definition.run !== 'function') {
    throw new Error(`Game library skill requires run(context): ${id}`)
  }

  return {
    ...definition,
    id,
    type: definition.type || 'extension',
    label: definition.label || id,
  }
}

function createExtensionCapability(skill, capability = {}) {
  return {
    skillId: skill.id,
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

function hasCapability(skillId, capabilities = getGameLibraryCapabilities()) {
  return capabilities.some((capability) => capability.skillId === skillId)
}

export function registerGameLibrarySkillExtension(extension = {}) {
  const skill = createGameLibrarySkill(extension.skill)

  if (hasCapability(skill.id)) {
    throw new Error(`Game library skill already registered: ${skill.id}`)
  }

  const entry = {
    skill,
    capability: createExtensionCapability(skill, extension.capability),
  }
  gameLibrarySkillExtensions.push(entry)

  return () => {
    const index = gameLibrarySkillExtensions.indexOf(entry)
    if (index !== -1) {
      gameLibrarySkillExtensions.splice(index, 1)
    }
  }
}

export function getGameLibraryCapabilities() {
  return [
    ...GAME_LIBRARY_AGENT_CAPABILITIES,
    ...gameLibrarySkillExtensions.map((extension) => extension.capability),
  ]
}

export function getGameLibraryCapability(skillId, capabilities = getGameLibraryCapabilities()) {
  return capabilities.find((capability) => capability.skillId === skillId) || null
}

export function getGameLibrarySelectableCapabilities(capabilities = getGameLibraryCapabilities()) {
  return capabilities.filter((capability) => capability.userSelectable)
}

export function getDefaultEnabledGameLibrarySkillIds(capabilities = getGameLibraryCapabilities()) {
  return capabilities
    .filter((capability) => capability.defaultEnabled || capability.required)
    .map((capability) => capability.skillId)
}

export function normalizeGameLibrarySkillIds(skillIds, capabilities = getGameLibraryCapabilities()) {
  const known = new Set(capabilities.map((capability) => capability.skillId))
  const enabled = Array.isArray(skillIds) ? skillIds.filter((skillId) => known.has(skillId)) : []
  const required = capabilities
    .filter((capability) => capability.required)
    .map((capability) => capability.skillId)

  return Array.from(new Set([...required, ...enabled]))
}

export function getGameLibraryCapabilityIcon(skillId, capabilities = getGameLibraryCapabilities()) {
  return getGameLibraryCapability(skillId, capabilities)?.icon || 'fa-bolt'
}

export function getGameLibraryCapabilityLabel(skillId, options = {}) {
  const capability = getGameLibraryCapability(skillId, options.capabilities)
  if (!capability) return skillId

  const locale = String(options.locale || '').toLowerCase()
  if (locale.startsWith('zh')) {
    return capability.labels?.zh || capability.label
  }

  return capability.label || skillId
}

export function createDefaultGameLibrarySkills(options = {}) {
  return [
    createScanOverrideMemorySkill(options.memory),
    createGameTitleNormalizeSkill(options.titleNormalize),
    createCoverSelectionSkill(options.coverSelection),
    ...gameLibrarySkillExtensions.map((extension) => extension.skill),
  ]
}

function filterSkills(skills, enabledSkills) {
  if (!Array.isArray(enabledSkills) || enabledSkills.length === 0) {
    return skills
  }

  const enabled = new Set(enabledSkills)
  return skills.filter((skill) => enabled.has(skill.id))
}

export function createGameLibraryCuratorAgent(options = {}) {
  const skills = options.skills || createDefaultGameLibrarySkills(options.skillsOptions)

  return {
    id: GAME_LIBRARY_AGENT_ID,
    skills,

    getSkill(skillId) {
      return skills.find((skill) => skill.id === skillId)
    },

    async run(apps, runOptions = {}) {
      let context = {
        apps,
        events: [],
        stats: {},
        options: runOptions,
      }

      for (const skill of filterSkills(skills, runOptions.enabledSkills)) {
        try {
          context = await skill.run(context)
        } catch (error) {
          context.options?.onSkillError?.(skill.id, error)
          context.events.push({
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

export async function runGameLibraryCuratorAgent(apps, options = {}) {
  return createGameLibraryCuratorAgent(options.agentOptions).run(apps, options)
}

const memorySkill = createScanOverrideMemorySkill()
const titleNormalizeSkill = createGameTitleNormalizeSkill()
const coverSelectionSkill = createCoverSelectionSkill()

export function applyGameLibraryOverrides(apps) {
  return memorySkill.apply(apps)
}

export function rememberGameLibraryApp(scannedApp, finalApp) {
  return memorySkill.remember(scannedApp, finalApp)
}

export async function enhanceGameLibraryMetadata(apps) {
  const result = await titleNormalizeSkill.run({ apps, events: [], stats: {}, options: {} })
  return result.apps
}

export async function findGameLibraryCover(app) {
  return coverSelectionSkill.findCover(app)
}
