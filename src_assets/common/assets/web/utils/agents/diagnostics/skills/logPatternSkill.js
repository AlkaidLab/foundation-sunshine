export const LOG_PATTERN_SKILL_ID = 'diagnostics.logs.patterns'

const LOG_PATTERNS = [
  {
    id: 'encoder-failure',
    severity: 'error',
    category: 'encoder',
    pattern: /\b(nvenc|amf|quicksync|video\s*toolbox|encoder|encoding)\b.*\b(fail|failed|error|unavailable|unsupported|unable|could not)\b/i,
    message: 'Encoder initialization or encoding failure detected',
    labels: { zh: '检测到编码器初始化或编码失败' },
  },
  {
    id: 'display-capture-failure',
    severity: 'error',
    category: 'display',
    pattern: /\b(display|monitor|dxgi|capture|duplication|output)\b.*\b(fail|failed|error|not found|unavailable|unable|could not)\b/i,
    message: 'Display capture or monitor detection failure detected',
    labels: { zh: '检测到显示捕获或显示器检测失败' },
  },
  {
    id: 'network-timeout',
    severity: 'warning',
    category: 'network',
    pattern: /\b(network|connection|client|pair|handshake|rtsp|udp|tcp|port)\b.*\b(timeout|timed out|refused|unreachable|blocked|failed|error)\b/i,
    message: 'Network, pairing, or client connection issue detected',
    labels: { zh: '检测到网络、配对或客户端连接问题' },
  },
  {
    id: 'port-bind-failure',
    severity: 'error',
    category: 'network',
    pattern: /\b(address already in use|permission denied|port\b.*\b(bind|listen|failed|error|unable|cannot)|\b(bind|listen)\b.*\b(port|failed|error|unable|cannot))\b/i,
    message: 'Port binding or listening failure detected',
    labels: { zh: '检测到端口绑定或监听失败' },
  },
  {
    id: 'config-risk',
    severity: 'warning',
    category: 'config',
    pattern: /\b(config|configuration|option|setting)\b.*\b(invalid|missing|conflict|deprecated|failed|error)\b/i,
    message: 'Configuration warning or invalid setting detected',
    labels: { zh: '检测到配置警告或无效设置' },
  },
]

function normalizeEvidence(line) {
  return String(line || '').trim().replace(/\s+/g, ' ')
}

export function findLogPatternFindings(logs = '') {
  const findingsByPattern = new Map()

  String(logs || '').split('\n').forEach((line, index) => {
    const evidence = normalizeEvidence(line)
    if (!evidence) return

    for (const pattern of LOG_PATTERNS) {
      if (!pattern.pattern.test(evidence)) continue

      const existing = findingsByPattern.get(pattern.id)
      if (existing) {
        existing.count += 1
        if (existing.evidence.length < 3) {
          existing.evidence.push({ lineNumber: index + 1, text: evidence })
        }
      } else {
        findingsByPattern.set(pattern.id, {
          id: `log-pattern:${pattern.id}`,
          type: pattern.id,
          category: pattern.category,
          severity: pattern.severity,
          skillId: LOG_PATTERN_SKILL_ID,
          message: pattern.message,
          labels: pattern.labels,
          count: 1,
          evidence: [{ lineNumber: index + 1, text: evidence }],
        })
      }
    }
  })

  return Array.from(findingsByPattern.values())
}

export function createLogPatternSkill(options = {}) {
  const findFindings = options.findFindings || findLogPatternFindings

  return {
    id: LOG_PATTERN_SKILL_ID,
    type: 'log-analysis',
    label: 'Log pattern detection',

    async run(context) {
      const findings = findFindings(context.logs || '')

      context.events?.push({
        skillId: LOG_PATTERN_SKILL_ID,
        type: 'logs:patterns-detected',
        findingsFound: findings.length,
      })

      return {
        ...context,
        findings: [...(context.findings || []), ...findings],
        stats: {
          ...(context.stats || {}),
          logPatternFindings: findings.length,
        },
      }
    },
  }
}
