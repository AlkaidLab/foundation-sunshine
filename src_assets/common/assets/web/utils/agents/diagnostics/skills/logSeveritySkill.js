export const LOG_SEVERITY_SKILL_ID = 'diagnostics.logs.severity'

const SEVERITY_PATTERNS = [
  { severity: 'fatal', pattern: /\b(fatal|critical|panic)\b/i },
  { severity: 'error', pattern: /\b(error|failed|failure|exception|unable to|could not)\b/i },
  { severity: 'warning', pattern: /\b(warn|warning|deprecated)\b/i },
]

function getLineSeverity(line) {
  return SEVERITY_PATTERNS.find((entry) => entry.pattern.test(line))?.severity || ''
}

export function summarizeLogSeverity(logs = '') {
  const counts = { fatal: 0, error: 0, warning: 0 }
  const evidence = []

  String(logs || '').split('\n').forEach((line, index) => {
    const severity = getLineSeverity(line)
    if (!severity) return

    counts[severity] += 1
    if (evidence.length < 8) {
      evidence.push({
        lineNumber: index + 1,
        severity,
        text: line.trim(),
      })
    }
  })

  return { counts, evidence }
}

export function createLogSeveritySkill(options = {}) {
  const summarize = options.summarize || summarizeLogSeverity

  return {
    id: LOG_SEVERITY_SKILL_ID,
    type: 'log-analysis',
    label: 'Log severity summary',

    async run(context) {
      const summary = summarize(context.logs || '')

      context.events?.push({
        skillId: LOG_SEVERITY_SKILL_ID,
        type: 'logs:severity-summarized',
        counts: summary.counts,
      })

      return {
        ...context,
        severitySummary: summary,
        stats: {
          ...(context.stats || {}),
          fatalLogLines: summary.counts.fatal,
          errorLogLines: summary.counts.error,
          warningLogLines: summary.counts.warning,
        },
      }
    },
  }
}
