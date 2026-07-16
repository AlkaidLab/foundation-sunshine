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
  createLogPatternSkill,
  LOG_PATTERN_SKILL_ID,
} from './skills/logPatternSkill.js'
import {
  createLogSeveritySkill,
  LOG_SEVERITY_SKILL_ID,
} from './skills/logSeveritySkill.js'
import {
  createLogRemediationSkill,
  LOG_REMEDIATION_SKILL_ID,
} from './skills/logRemediationSkill.js'

export const DIAGNOSTICS_AGENT_ID = 'diagnostics'

export const DIAGNOSTICS_SKILL_IDS = {
  logSeverity: LOG_SEVERITY_SKILL_ID,
  logPatterns: LOG_PATTERN_SKILL_ID,
  logRemediation: LOG_REMEDIATION_SKILL_ID,
}

export const DIAGNOSTICS_CAPABILITIES = [
  {
    skillId: LOG_SEVERITY_SKILL_ID,
    stage: 'log-analysis',
    icon: 'fa-chart-simple',
    label: 'Log severity summary',
    labels: {
      zh: '日志严重度摘要',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
  {
    skillId: LOG_PATTERN_SKILL_ID,
    stage: 'log-analysis',
    icon: 'fa-magnifying-glass-chart',
    label: 'Log pattern detection',
    labels: {
      zh: '日志模式检测',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
  {
    skillId: LOG_REMEDIATION_SKILL_ID,
    stage: 'log-analysis',
    icon: 'fa-screwdriver-wrench',
    label: 'Log remediation suggestions',
    labels: {
      zh: '日志修复建议',
    },
    defaultEnabled: true,
    userSelectable: true,
  },
]

export function createDiagnosticsSkill(definition = {}) {
  return createAgentSkill(definition, {
    skillSubject: 'Diagnostics skills',
    runSubject: 'Diagnostics skill',
  })
}

const diagnosticsRegistry = createSkillRegistry({
  baseCapabilities: DIAGNOSTICS_CAPABILITIES,
  createSkill: createDiagnosticsSkill,
  duplicateMessage: 'Diagnostics skill already registered',
})

export function registerDiagnosticsSkillExtension(extension = {}) {
  return diagnosticsRegistry.registerExtension(extension)
}

export function getDiagnosticsCapabilities() {
  return diagnosticsRegistry.getCapabilities()
}

export function getDiagnosticsCapability(skillId, capabilities = getDiagnosticsCapabilities()) {
  return getAgentCapability(skillId, capabilities)
}

export function getDiagnosticsSelectableCapabilities(capabilities = getDiagnosticsCapabilities()) {
  return getSelectableAgentCapabilities(capabilities)
}

export function getDefaultEnabledDiagnosticsSkillIds(capabilities = getDiagnosticsCapabilities()) {
  return getDefaultEnabledSkillIds(capabilities)
}

export function normalizeDiagnosticsSkillIds(skillIds, capabilities = getDiagnosticsCapabilities()) {
  return normalizeEnabledSkillIds(skillIds, capabilities)
}

export function getDiagnosticsCapabilityIcon(skillId, capabilities = getDiagnosticsCapabilities()) {
  return getAgentCapabilityIcon(skillId, capabilities)
}

export function getDiagnosticsCapabilityLabel(skillId, options = {}) {
  return getAgentCapabilityLabel(skillId, {
    ...options,
    capabilities: options.capabilities || getDiagnosticsCapabilities(),
  })
}

export function createDefaultDiagnosticsSkills(options = {}) {
  return [
    createLogSeveritySkill(options.logSeverity),
    createLogPatternSkill(options.logPatterns),
    createLogRemediationSkill(options.logRemediation),
    ...diagnosticsRegistry.getExtensionSkills(),
  ]
}

export function createDiagnosticsAgent(options = {}) {
  const skills = options.skills || createDefaultDiagnosticsSkills(options.skillsOptions)

  return createAgent({
    id: DIAGNOSTICS_AGENT_ID,
    skills,
    createContext(logs, runOptions) {
      return {
        logs,
        findings: [],
        suggestions: [],
        severitySummary: null,
        events: [],
        stats: {},
        options: runOptions,
      }
    },
  })
}

export async function runDiagnosticsAgent(logs, options = {}) {
  return createDiagnosticsAgent(options.agentOptions).run(logs, options)
}
