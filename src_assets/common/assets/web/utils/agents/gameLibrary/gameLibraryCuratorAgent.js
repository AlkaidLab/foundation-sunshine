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
    defaultEnabled: true,
    userSelectable: false,
  },
  {
    skillId: GAME_TITLE_NORMALIZE_SKILL_ID,
    stage: 'metadata',
    defaultEnabled: true,
    userSelectable: true,
  },
  {
    skillId: COVER_SELECTION_SKILL_ID,
    stage: 'asset',
    defaultEnabled: true,
    userSelectable: true,
  },
]

export function createDefaultGameLibrarySkills(options = {}) {
  return [
    createScanOverrideMemorySkill(options.memory),
    createGameTitleNormalizeSkill(options.titleNormalize),
    createCoverSelectionSkill(options.coverSelection),
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
        context = await skill.run(context)
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
