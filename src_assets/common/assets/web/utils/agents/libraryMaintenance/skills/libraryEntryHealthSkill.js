export const LIBRARY_ENTRY_HEALTH_SKILL_ID = 'library.entries.health'

function hasText(value) {
  return typeof value === 'string' && value.trim().length > 0
}

function createIssue(app, index, type, severity, message, zhLabel) {
  return {
    id: `entry:${index}:${type}`,
    type,
    severity,
    skillId: LIBRARY_ENTRY_HEALTH_SKILL_ID,
    appIndexes: [index],
    message,
    labels: {
      zh: zhLabel,
    },
    evidence: {
      name: app?.name || '',
      cmd: app?.cmd || '',
      imagePath: app?.['image-path'] || '',
    },
  }
}

export function findLibraryEntryHealthIssues(apps = []) {
  const issues = []

  apps.forEach((app, index) => {
    if (!hasText(app?.name)) {
      issues.push(createIssue(
        app,
        index,
        'missing-name',
        'error',
        'Application entry is missing a display name',
        '应用条目缺少显示名称'
      ))
    }

    if (!hasText(app?.cmd)) {
      issues.push(createIssue(
        app,
        index,
        'missing-command',
        'error',
        'Application entry is missing a launch command',
        '应用条目缺少启动命令'
      ))
    }

    if (!hasText(app?.['image-path'])) {
      issues.push(createIssue(
        app,
        index,
        'missing-cover',
        'info',
        'Application entry has no cover image',
        '应用条目缺少封面'
      ))
    }
  })

  return issues
}

export function createLibraryEntryHealthSkill(options = {}) {
  const findIssues = options.findIssues || findLibraryEntryHealthIssues

  return {
    id: LIBRARY_ENTRY_HEALTH_SKILL_ID,
    type: 'quality',
    label: 'Library entry health check',

    async run(context) {
      const issues = findIssues(context.apps || [])

      context.events?.push({
        skillId: LIBRARY_ENTRY_HEALTH_SKILL_ID,
        type: 'entries:checked',
        issuesFound: issues.length,
      })

      return {
        ...context,
        issues: [...(context.issues || []), ...issues],
        stats: {
          ...(context.stats || {}),
          entryHealthIssues: issues.length,
        },
      }
    },
  }
}
