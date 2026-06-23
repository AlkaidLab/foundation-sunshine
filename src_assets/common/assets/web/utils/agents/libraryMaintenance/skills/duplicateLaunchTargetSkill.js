export const DUPLICATE_LAUNCH_TARGET_SKILL_ID = 'library.duplicates.launch-target'

function normalizeLaunchValue(value) {
  return String(value || '')
    .replace(/\\/g, '/')
    .replace(/\s+/g, ' ')
    .trim()
    .toLowerCase()
}

function getLaunchKey(app) {
  const cmd = normalizeLaunchValue(app?.cmd)
  if (!cmd) return ''

  return [
    cmd,
    normalizeLaunchValue(app?.['working-dir'] || app?.working_dir),
  ].join('|')
}

function createDuplicateIssue(key, entries) {
  const [cmd, workingDir] = key.split('|')

  return {
    id: `duplicate-launch-target:${key}`,
    type: 'duplicate-launch-target',
    severity: 'warning',
    skillId: DUPLICATE_LAUNCH_TARGET_SKILL_ID,
    appIndexes: entries.map((entry) => entry.index),
    message: `Duplicate launch target used by ${entries.length} apps`,
    labels: {
      zh: `${entries.length} 个应用使用相同启动目标`,
    },
    evidence: {
      cmd,
      workingDir,
      names: entries.map((entry) => entry.app?.name || ''),
    },
  }
}

export function findDuplicateLaunchTargetIssues(apps = []) {
  const groups = new Map()

  apps.forEach((app, index) => {
    const key = getLaunchKey(app)
    if (!key) return

    const entries = groups.get(key) || []
    entries.push({ app, index })
    groups.set(key, entries)
  })

  return Array.from(groups.entries())
    .filter(([, entries]) => entries.length > 1)
    .map(([key, entries]) => createDuplicateIssue(key, entries))
}

export function createDuplicateLaunchTargetSkill(options = {}) {
  const findIssues = options.findIssues || findDuplicateLaunchTargetIssues

  return {
    id: DUPLICATE_LAUNCH_TARGET_SKILL_ID,
    type: 'quality',
    label: 'Duplicate launch target detection',

    async run(context) {
      const issues = findIssues(context.apps || [])

      context.events?.push({
        skillId: DUPLICATE_LAUNCH_TARGET_SKILL_ID,
        type: 'duplicates:checked',
        issuesFound: issues.length,
      })

      return {
        ...context,
        issues: [...(context.issues || []), ...issues],
        stats: {
          ...(context.stats || {}),
          duplicateLaunchTargets: issues.length,
        },
      }
    },
  }
}
