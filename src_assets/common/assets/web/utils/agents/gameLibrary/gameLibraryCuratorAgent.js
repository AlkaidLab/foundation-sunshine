import { createCoverSelectionSkill, COVER_SELECTION_SKILL_ID } from './skills/coverSelectionSkill.js'
import { createGameTitleNormalizeSkill, GAME_TITLE_NORMALIZE_SKILL_ID } from './skills/gameTitleNormalizeSkill.js'
import { createScanOverrideMemorySkill, SCAN_OVERRIDE_MEMORY_SKILL_ID } from './skills/scanOverrideMemorySkill.js'
import {
  createAgent,
  createAgentSkill,
  createSkillRegistry,
  getAgentCapability,
  getAgentCapabilityIcon,
  getAgentCapabilityLabel,
  getDefaultEnabledSkillIds,
  getSelectableAgentCapabilities,
  normalizeEnabledSkillIds,
} from '../core/agentCore.js'

export { getGameResourceKey } from './skills/coverSelectionSkill.js'
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

export function createGameLibrarySkill(definition = {}) {
  return createAgentSkill(definition, {
    skillSubject: 'Game library skills',
    runSubject: 'Game library skill',
  })
}

const gameLibraryRegistry = createSkillRegistry({
  baseCapabilities: GAME_LIBRARY_AGENT_CAPABILITIES,
  createSkill: createGameLibrarySkill,
  duplicateMessage: 'Game library skill already registered',
})

export function registerGameLibrarySkillExtension(extension = {}) {
  return gameLibraryRegistry.registerExtension(extension)
}

export function getGameLibraryCapabilities() {
  return gameLibraryRegistry.getCapabilities()
}

export function getGameLibraryCapability(skillId, capabilities = getGameLibraryCapabilities()) {
  return getAgentCapability(skillId, capabilities)
}

export function getGameLibrarySelectableCapabilities(capabilities = getGameLibraryCapabilities()) {
  return getSelectableAgentCapabilities(capabilities)
}

export function getDefaultEnabledGameLibrarySkillIds(capabilities = getGameLibraryCapabilities()) {
  return getDefaultEnabledSkillIds(capabilities)
}

export function normalizeGameLibrarySkillIds(skillIds, capabilities = getGameLibraryCapabilities()) {
  return normalizeEnabledSkillIds(skillIds, capabilities)
}

export function getGameLibraryCapabilityIcon(skillId, capabilities = getGameLibraryCapabilities()) {
  return getAgentCapabilityIcon(skillId, capabilities)
}

export function getGameLibraryCapabilityLabel(skillId, options = {}) {
  return getAgentCapabilityLabel(skillId, {
    ...options,
    capabilities: options.capabilities || getGameLibraryCapabilities(),
  })
}

export function createDefaultGameLibrarySkills(options = {}) {
  return [
    createScanOverrideMemorySkill(options.memory),
    createGameTitleNormalizeSkill(options.titleNormalize),
    createCoverSelectionSkill(options.coverSelection),
    ...gameLibraryRegistry.getExtensionSkills(),
  ]
}

export function createGameLibraryCuratorAgent(options = {}) {
  const skills = options.skills || createDefaultGameLibrarySkills(options.skillsOptions)

  return createAgent({
    id: GAME_LIBRARY_AGENT_ID,
    skills,
    createContext(apps, runOptions) {
      return {
        apps,
        events: [],
        stats: {},
        options: runOptions,
      }
    },
  })
}

export async function runGameLibraryCuratorAgent(apps, options = {}) {
  return createGameLibraryCuratorAgent(options.agentOptions).run(apps, options)
}

const memorySkill = createScanOverrideMemorySkill()
const titleNormalizeSkill = createGameTitleNormalizeSkill()

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
