import { ref, reactive } from 'vue'
import { API_ENDPOINTS } from '../utils/constants.js'
import { buildLocalizedInstruction, getCurrentLocale, getPromptLanguageName } from '../utils/aiLocale.js'
import { diagnosisRequestBody, openLogDiagnosis } from 'foundation-ui/diagnostics'
import { fetchAiJson } from '../utils/aiProxyFetch.js'

const DEFAULT_CONFIG = {
  enabled: false,
  provider: 'openai',
  apiBase: 'https://api.openai.com/v1',
  apiKey: '',
  apiKeyConfigured: false,
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
      const remote = await fetchAiJson(API_ENDPOINTS.AI_CONFIG)
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
    // The shared AI config exposes only key presence; the proxy reads the secret.
    if (!(config.apiKeyConfigured || config.apiKey) && isApiKeyRequired(config)) {
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

  /** @param {{ context?: string }} [options] context: text the analysis reads before the log (the result of 体检) */
  async function diagnose(logs, { context = '' } = {}) {
    if (!logs) {
      error.value = 'No log content is available.'
      return
    }

    const prepared = openLogDiagnosis(logs)
    localFindings.value = prepared.findings
    localSuggestions.value = prepared.suggestions

    await loadConfig()
    const validationError = validateConfig()
    if (validationError) {
      error.value = validationError
      return
    }

    isLoading.value = true
    result.value = ''
    error.value = ''
    try {
      const data = await fetchAiJson(API_ENDPOINTS.AI_CHAT_COMPLETIONS, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(diagnosisRequestBody({
          model: config.model,
          requestText: context ? ['Host health check (体检) result:', '```', String(context).slice(0, 12000), '```', '', prepared.requestText].join('\n') : prepared.requestText,
          system: buildSystemPrompt(),
          temperature: config.temperature,
          maxTokens: config.max_tokens,
        })),
      })
      result.value = data.choices?.[0]?.message?.content || 'Unable to read the analysis result.'
    } catch (e) {
      error.value = e.message
    } finally {
      isLoading.value = false
    }
  }

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
