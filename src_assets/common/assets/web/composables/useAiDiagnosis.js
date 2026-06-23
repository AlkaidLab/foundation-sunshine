import { ref, reactive } from 'vue'
import { API_ENDPOINTS } from '../utils/constants.js'
import { buildLocalizedInstruction, getCurrentLocale, getPromptLanguageName } from '../utils/aiLocale.js'
import { runDiagnosticsAgent } from '../utils/agents/diagnostics/diagnosticsAgent.js'

const DEFAULT_CONFIG = {
  enabled: false,
  provider: 'openai',
  apiBase: 'https://api.openai.com/v1',
  apiKey: '',
  model: 'gpt-4.1-mini',
  compatibility: 'openai-chat',
  temperature: 0.3,
  max_tokens: 2048,
}

function buildSystemPrompt(locale = getCurrentLocale()) {
  const languageName = getPromptLanguageName(locale)

  return `You are a Sunshine game streaming log diagnosis assistant.

Analyze the provided Sunshine logs and reply in concise ${languageName}.
${buildLocalizedInstruction(locale)}

Focus on:
- Fatal/Error lines as likely direct causes.
- Warning lines as useful symptoms.
- Encoder issues involving NVENC, AMF, QuickSync, VideoToolbox, or software encoding.
- Network, pairing, timeout, and Moonlight client connection problems.
- Audio/video capture pipeline failures.
- Invalid or conflicting configuration.

Use this structure:
1. Problem summary
2. Detailed analysis with concrete log evidence
3. Actionable fixes
4. If no obvious error is present, say the current logs look normal.`
}

function normalizeConfig(input = {}) {
  const cfg = { ...DEFAULT_CONFIG, ...input }
  if (!cfg.compatibility) {
    cfg.compatibility = DEFAULT_CONFIG.compatibility
  }
  return cfg
}

function isApiKeyRequired(cfg) {
  const apiBase = cfg.apiBase || ''
  return cfg.provider !== 'ollama' &&
    !apiBase.includes('localhost') &&
    !apiBase.includes('127.0.0.1') &&
    !apiBase.includes('[::1]')
}

function buildLocalDiagnosticsSummary(diagnostics) {
  const findings = diagnostics?.findings || []
  const suggestions = diagnostics?.suggestions || []
  const severity = diagnostics?.severitySummary?.counts || {}

  return JSON.stringify({
    severity,
    findings: findings.map((finding) => ({
      type: finding.type,
      category: finding.category,
      severity: finding.severity,
      message: finding.message,
      count: finding.count,
      evidence: (finding.evidence || []).slice(0, 3),
    })),
    suggestions: suggestions.map((suggestion) => ({
      findingType: suggestion.findingType,
      severity: suggestion.severity,
      title: suggestion.title,
      actions: suggestion.actions,
    })),
  }, null, 2)
}

async function fetchJson(url, options = {}) {
  const resp = await fetch(url, options)
  const data = await resp.json().catch(() => ({}))
  if (!resp.ok || data.status === 'error') {
    const message = typeof data.error === 'string' ? data.error : data.error?.message
    throw new Error(message || `Request failed: ${resp.status}`)
  }
  return data
}

export function useAiDiagnosis() {
  const config = reactive({ ...DEFAULT_CONFIG })
  const isConfigLoading = ref(false)
  const isSavingConfig = ref(false)
  const isLoading = ref(false)
  const result = ref('')
  const error = ref('')
  const localFindings = ref([])
  const localSuggestions = ref([])
  const localSeveritySummary = ref(null)

  async function loadConfig() {
    isConfigLoading.value = true
    error.value = ''

    try {
      const remote = await fetchJson(API_ENDPOINTS.AI_CONFIG)
      Object.assign(config, normalizeConfig(remote))

    } catch (e) {
      Object.assign(config, normalizeConfig(DEFAULT_CONFIG))
      error.value = e.message
    } finally {
      isConfigLoading.value = false
    }
  }

  function validateConfig() {
    if (!config.enabled) {
      return 'Please enable the local AI proxy first.'
    }
    if (!config.apiBase) {
      return 'Please configure an API Base first.'
    }
    if (!config.model) {
      return 'Please configure a model first.'
    }
    if (!config.apiKey && isApiKeyRequired(config)) {
      return 'Please configure an API key first.'
    }
    if (config.provider === 'custom' || config.provider === 'ollama') {
      try {
        const url = new URL(config.apiBase)
        if (!['http:', 'https:'].includes(url.protocol)) {
          throw new Error('invalid protocol')
        }
      } catch {
        return 'Custom providers need a complete API Base that starts with http:// or https://.'
      }
    }
    return ''
  }

  async function diagnose(logs) {
    if (!logs) {
      error.value = 'No log content is available.'
      return
    }

    const diagnostics = await runDiagnosticsAgent(logs)
    localFindings.value = diagnostics.findings || []
    localSuggestions.value = diagnostics.suggestions || []
    localSeveritySummary.value = diagnostics.severitySummary || null

    const validationError = validateConfig()
    if (validationError) {
      error.value = validationError
      return
    }

    isLoading.value = true
    result.value = ''
    error.value = ''

    const lines = logs.split('\n')
    const truncated = lines.slice(-200).join('\n')
    const localSummary = buildLocalDiagnosticsSummary({
      findings: localFindings.value,
      suggestions: localSuggestions.value,
      severitySummary: localSeveritySummary.value,
    })

    try {
      const data = await fetchJson(API_ENDPOINTS.AI_CHAT_COMPLETIONS, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model: config.model,
          messages: [
            { role: 'system', content: buildSystemPrompt() },
            {
              role: 'user',
              content: [
                'Local rule-based pre-diagnosis:',
                '```json',
                localSummary,
                '```',
                '',
                'Please analyze these Sunshine logs:',
                '```',
                truncated,
                '```',
              ].join('\n'),
            },
          ],
          temperature: Number(config.temperature) || 0.3,
          max_tokens: Number(config.max_tokens) || 2048,
        }),
      })

      result.value = data.choices?.[0]?.message?.content || 'Unable to read the analysis result.'
    } catch (e) {
      error.value = e.message
    } finally {
      isLoading.value = false
    }
  }

  loadConfig()

  return {
    config,
    isConfigLoading,
    isSavingConfig,
    isLoading,
    result,
    error,
    localFindings,
    localSuggestions,
    localSeveritySummary,
    diagnose,
    loadConfig,
  }
}
