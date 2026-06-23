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
import {
  createDuplicateLaunchTargetSkill,
  DUPLICATE_LAUNCH_TARGET_SKILL_ID,
} from './skills/duplicateLaunchTargetSkill.js'
import {
  createLibraryEntryHealthSkill,
  LIBRARY_ENTRY_HEALTH_SKILL_ID,
} from './skills/libraryEntryHealthSkill.js'

export const LIBRARY_MAINTENANCE_AGENT_ID = 'library-maintenance'

export const LIBRARY_MAINTENANCE_SKILL_IDS = {
  duplicateLaunchTargets: DUPLICATE_LAUNCH_TARGET_SKILL_ID,
  entryHealth: LIBRARY_ENTRY_HEALTH_SKILL_ID,
}

export const LIBRARY_MAINTENANCE_CAPABILITIES = [
  {
    skillId: DUPLICATE_LAUNCH_TARGET_SKILL_ID,
    stage: 'quality',
    icon: 'fa-copy',
    label: 'Duplicate launch targets',
    labels: {
      zh: '重复启动目标',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
  {
    skillId: LIBRARY_ENTRY_HEALTH_SKILL_ID,
    stage: 'quality',
    icon: 'fa-list-check',
    label: 'Entry health checks',
    labels: {
      zh: '条目健康检查',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
]

export function createLibraryMaintenanceSkill(definition = {}) {
  return createAgentSkill(definition, {
    skillSubject: 'Library maintenance skills',
    runSubject: 'Library maintenance skill',
  })
}

const libraryMaintenanceRegistry = createSkillRegistry({
  baseCapabilities: LIBRARY_MAINTENANCE_CAPABILITIES,
  createSkill: createLibraryMaintenanceSkill,
  duplicateMessage: 'Library maintenance skill already registered',
})

export function registerLibraryMaintenanceSkillExtension(extension = {}) {
  return libraryMaintenanceRegistry.registerExtension(extension)
}

export function getLibraryMaintenanceCapabilities() {
  return libraryMaintenanceRegistry.getCapabilities()
}

export function getLibraryMaintenanceCapability(skillId, capabilities = getLibraryMaintenanceCapabilities()) {
  return getAgentCapability(skillId, capabilities)
}

export function getLibraryMaintenanceSelectableCapabilities(capabilities = getLibraryMaintenanceCapabilities()) {
  return getSelectableAgentCapabilities(capabilities)
}

export function getDefaultEnabledLibraryMaintenanceSkillIds(capabilities = getLibraryMaintenanceCapabilities()) {
  return getDefaultEnabledSkillIds(capabilities)
}

export function normalizeLibraryMaintenanceSkillIds(skillIds, capabilities = getLibraryMaintenanceCapabilities()) {
  return normalizeEnabledSkillIds(skillIds, capabilities)
}

export function getLibraryMaintenanceCapabilityIcon(skillId, capabilities = getLibraryMaintenanceCapabilities()) {
  return getAgentCapabilityIcon(skillId, capabilities)
}

export function getLibraryMaintenanceCapabilityLabel(skillId, options = {}) {
  return getAgentCapabilityLabel(skillId, {
    ...options,
    capabilities: options.capabilities || getLibraryMaintenanceCapabilities(),
  })
}

export function createDefaultLibraryMaintenanceSkills(options = {}) {
  return [
    createDuplicateLaunchTargetSkill(options.duplicates),
    createLibraryEntryHealthSkill(options.entryHealth),
    ...libraryMaintenanceRegistry.getExtensionSkills(),
  ]
}

export function createLibraryMaintenanceAgent(options = {}) {
  const skills = options.skills || createDefaultLibraryMaintenanceSkills(options.skillsOptions)

  return createAgent({
    id: LIBRARY_MAINTENANCE_AGENT_ID,
    skills,
    createContext(apps, runOptions) {
      return {
        apps,
        issues: [],
        events: [],
        stats: {},
        options: runOptions,
      }
    },
  })
}

export async function runLibraryMaintenanceAgent(apps, options = {}) {
  return createLibraryMaintenanceAgent(options.agentOptions).run(apps, options)
}
