export const LOG_REMEDIATION_SKILL_ID = 'diagnostics.logs.remediation'

const REMEDIATION_BY_TYPE = {
  'encoder-failure': {
    severity: 'error',
    title: 'Check encoder availability and GPU driver support',
    labels: { zh: '检查编码器可用性和显卡驱动支持' },
    actions: [
      'Confirm the selected encoder is supported by the current GPU and driver.',
      'Try switching to another hardware encoder or software encoding as a temporary fallback.',
      'Update the GPU driver, then restart Sunshine.',
    ],
    actionLabels: {
      zh: [
        '确认当前显卡和驱动支持所选编码器。',
        '临时切换到其他硬件编码器或软件编码。',
        '更新显卡驱动后重启 Sunshine。',
      ],
    },
  },
  'display-capture-failure': {
    severity: 'error',
    title: 'Verify display selection and capture availability',
    labels: { zh: '检查显示器选择和捕获可用性' },
    actions: [
      'Open the Troubleshooting page and reset display device persistence if the display changed recently.',
      'Confirm the selected monitor is connected, enabled, and visible to the system.',
      'Restart Sunshine after changing display or GPU settings.',
    ],
    actionLabels: {
      zh: [
        '如果最近更换过显示器，在故障排查页重置显示设备持久化。',
        '确认所选显示器已连接、启用，并且系统可见。',
        '修改显示器或显卡设置后重启 Sunshine。',
      ],
    },
  },
  'network-timeout': {
    severity: 'warning',
    title: 'Check client connectivity, firewall, and network path',
    labels: { zh: '检查客户端连接、防火墙和网络路径' },
    actions: [
      'Confirm the Moonlight client can reach the Sunshine host IP.',
      'Check firewall rules for Sunshine and the configured streaming ports.',
      'Retry pairing after confirming both devices are on the expected network.',
    ],
    actionLabels: {
      zh: [
        '确认 Moonlight 客户端可以访问 Sunshine 主机 IP。',
        '检查 Sunshine 和串流端口的防火墙规则。',
        '确认两台设备在预期网络后重新配对。',
      ],
    },
  },
  'port-bind-failure': {
    severity: 'error',
    title: 'Free or change the occupied Sunshine port',
    labels: { zh: '释放或修改被占用的 Sunshine 端口' },
    actions: [
      'Check whether another Sunshine instance or service is already listening on the same port.',
      'Stop the conflicting process or change the Sunshine port configuration.',
      'Run Sunshine with sufficient permissions if the log mentions permission denied.',
    ],
    actionLabels: {
      zh: [
        '检查是否已有另一个 Sunshine 实例或服务监听同一端口。',
        '停止冲突进程，或修改 Sunshine 端口配置。',
        '如果日志提示权限不足，请使用足够权限运行 Sunshine。',
      ],
    },
  },
  'config-risk': {
    severity: 'warning',
    title: 'Review recently changed Sunshine configuration',
    labels: { zh: '检查最近修改过的 Sunshine 配置' },
    actions: [
      'Review the setting mentioned near the warning or error line.',
      'Restore the option to a known working value if the issue started after a config change.',
      'Save the config and restart Sunshine after changing related settings.',
    ],
    actionLabels: {
      zh: [
        '检查警告或错误附近提到的配置项。',
        '如果问题出现在改配置之后，先恢复到已知可用值。',
        '修改相关配置后保存并重启 Sunshine。',
      ],
    },
  },
}

export function createRemediationSuggestions(findings = []) {
  const suggestionsByType = new Map()

  for (const finding of findings) {
    const template = REMEDIATION_BY_TYPE[finding?.type]
    if (!template || suggestionsByType.has(finding.type)) continue

    suggestionsByType.set(finding.type, {
      id: `remediation:${finding.type}`,
      type: 'remediation',
      findingType: finding.type,
      category: finding.category || 'general',
      severity: template.severity || finding.severity || 'info',
      skillId: LOG_REMEDIATION_SKILL_ID,
      title: template.title,
      labels: template.labels || {},
      actions: template.actions || [],
      actionLabels: template.actionLabels || {},
      evidence: finding.evidence || [],
    })
  }

  return Array.from(suggestionsByType.values())
}

export function createLogRemediationSkill(options = {}) {
  const createSuggestions = options.createSuggestions || createRemediationSuggestions

  return {
    id: LOG_REMEDIATION_SKILL_ID,
    type: 'log-analysis',
    label: 'Log remediation suggestions',

    async run(context) {
      const suggestions = createSuggestions(context.findings || [])

      context.events?.push({
        skillId: LOG_REMEDIATION_SKILL_ID,
        type: 'logs:remediation-suggested',
        suggestionsFound: suggestions.length,
      })

      return {
        ...context,
        suggestions: [...(context.suggestions || []), ...suggestions],
        stats: {
          ...(context.stats || {}),
          logRemediationSuggestions: suggestions.length,
        },
      }
    },
  }
}
