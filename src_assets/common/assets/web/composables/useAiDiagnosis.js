import { ref, reactive } from 'vue'
import { API_ENDPOINTS } from '../utils/constants.js'
import { buildLocalizedInstruction, getCurrentLocale, getPromptLanguageName } from '../utils/aiLocale.js'

const LEGACY_STORAGE_KEY = 'sunshine-ai-diagnosis-config'

const PROVIDERS = [
  {
    label: 'OpenAI',
    value: 'openai',
    base: 'https://api.openai.com/v1',
    compatibility: 'openai-chat',
    models: ['gpt-4.1-mini', 'gpt-4.1', 'gpt-4o-mini'],
  },
  {
    label: 'DeepSeek',
    value: 'deepseek',
    base: 'https://api.deepseek.com/v1',
    compatibility: 'openai-chat',
    models: ['deepseek-chat', 'deepseek-reasoner'],
  },
  {
    label: 'Qwen',
    value: 'qwen',
    base: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    compatibility: 'openai-chat',
    models: ['qwen-plus', 'qwen-turbo', 'qwen-max'],
  },
  {
    label: 'Gemini',
    value: 'gemini',
    base: 'https://generativelanguage.googleapis.com/v1beta/openai',
    compatibility: 'openai-chat',
    models: ['gemini-2.5-flash', 'gemini-2.5-pro'],
  },
  {
    label: 'Anthropic',
    value: 'anthropic',
    base: 'https://api.anthropic.com',
    compatibility: 'anthropic-messages',
    models: ['claude-3-5-haiku-latest', 'claude-sonnet-4-5'],
  },
  {
    label: 'OpenRouter',
    value: 'openrouter',
    base: 'https://openrouter.ai/api/v1',
    compatibility: 'openai-chat',
    models: ['openai/gpt-4.1-mini', 'anthropic/claude-sonnet-4.5', 'deepseek/deepseek-chat-v3.1'],
  },
  {
    label: 'Ollama',
    value: 'ollama',
    base: 'http://localhost:11434/v1',
    compatibility: 'openai-chat',
    models: ['llama3.1', 'qwen2.5', 'gemma3'],
  },
  {
    label: 'Mita / Custom',
    value: 'custom',
    base: '',
    compatibility: 'openai-chat',
    models: [],
  },
]

const COMPATIBILITY_OPTIONS = [
  { label: 'OpenAI Chat Completions', value: 'openai-chat' },
  { label: 'Anthropic Messages', value: 'anthropic-messages' },
]

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
  const provider = PROVIDERS.find((p) => p.value === cfg.provider)
  if (!cfg.compatibility) {
    cfg.compatibility = provider?.compatibility || 'openai-chat'
  }
  return cfg
}

function isMaskedKey(key) {
  return typeof key === 'string' && key.includes('****')
}

function isApiKeyRequired(cfg) {
  const apiBase = cfg.apiBase || ''
  return cfg.provider !== 'ollama' &&
    !apiBase.includes('localhost') &&
    !apiBase.includes('127.0.0.1') &&
    !apiBase.includes('[::1]')
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

function loadLegacyConfig() {
  try {
    const saved = localStorage.getItem(LEGACY_STORAGE_KEY)
    return saved ? JSON.parse(saved) : null
  } catch {
    return null
  }
}

export function useAiDiagnosis() {
  const config = reactive({ ...DEFAULT_CONFIG })
  const isConfigLoading = ref(false)
  const isSavingConfig = ref(false)
  const isLoading = ref(false)
  const result = ref('')
  const error = ref('')

  async function loadConfig() {
    isConfigLoading.value = true
    error.value = ''

    try {
      const remote = await fetchJson(API_ENDPOINTS.AI_CONFIG)
      Object.assign(config, normalizeConfig(remote))

      const legacy = loadLegacyConfig()
      if (legacy && !remote.enabled && legacy.apiBase && legacy.model) {
        Object.assign(config, normalizeConfig({ ...config, ...legacy, enabled: true }))
        await saveConfig()
        localStorage.removeItem(LEGACY_STORAGE_KEY)
      }
    } catch (e) {
      Object.assign(config, normalizeConfig(loadLegacyConfig() || DEFAULT_CONFIG))
      error.value = e.message
    } finally {
      isConfigLoading.value = false
    }
  }

  async function saveConfig() {
    isSavingConfig.value = true
    try {
      const body = { ...config }
      if (isMaskedKey(body.apiKey)) {
        delete body.apiKey
      }
      await fetchJson(API_ENDPOINTS.AI_CONFIG, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
    } finally {
      isSavingConfig.value = false
    }
  }

  function onProviderChange(value) {
    const p = PROVIDERS.find((x) => x.value === value)
    if (p) {
      config.apiBase = p.base
      config.compatibility = p.compatibility
      if (p.models.length > 0) config.model = p.models[0]
    }
  }

  function getAvailableModels() {
    const p = PROVIDERS.find((x) => x.value === config.provider)
    return p?.models || []
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

    try {
      await saveConfig()

      const data = await fetchJson(API_ENDPOINTS.AI_CHAT_COMPLETIONS, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          model: config.model,
          messages: [
            { role: 'system', content: buildSystemPrompt() },
            { role: 'user', content: `Please analyze these Sunshine logs:\n\n\`\`\`\n${truncated}\n\`\`\`` },
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
    providers: PROVIDERS,
    compatibilityOptions: COMPATIBILITY_OPTIONS,
    isConfigLoading,
    isSavingConfig,
    isLoading,
    result,
    error,
    onProviderChange,
    getAvailableModels,
    diagnose,
    saveConfig,
    loadConfig,
  }
}
