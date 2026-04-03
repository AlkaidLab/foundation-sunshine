import { ref, reactive } from 'vue'

const STORAGE_KEY = 'sunshine-ai-diagnosis-config'

const PROVIDERS = [
  { label: 'OpenAI', value: 'openai', base: 'https://api.openai.com/v1', models: ['gpt-4o-mini', 'gpt-4o'] },
  { label: 'DeepSeek', value: 'deepseek', base: 'https://api.deepseek.com/v1', models: ['deepseek-chat'] },
  { label: '通义千问', value: 'qwen', base: 'https://dashscope.aliyuncs.com/compatible-mode/v1', models: ['qwen-plus', 'qwen-turbo'] },
  { label: '智谱 (GLM)', value: 'glm', base: 'https://open.bigmodel.cn/api/paas/v4', models: ['glm-4-flash', 'glm-4'] },
  { label: 'OpenRouter', value: 'openrouter', base: 'https://openrouter.ai/api/v1', models: ['deepseek/deepseek-chat-v3-0324', 'google/gemini-2.0-flash-001'] },
  { label: 'Ollama (本地)', value: 'ollama', base: 'http://localhost:11434/v1', models: ['llama3', 'qwen2'] },
  { label: '自定义', value: 'custom', base: '', models: [] },
]

const SYSTEM_PROMPT = `你是 Sunshine 串流软件的日志诊断助手。用户会提供 Sunshine 的运行日志，请分析日志内容并给出诊断结果。

请关注以下内容：
- **Fatal/Error 级别日志**：通常是问题的直接原因
- **Warning 日志**：可能暗示潜在问题
- **编码器相关**：NVENC/AMF/软件编码的错误或回退
- **网络/连接**：Moonlight 客户端连接失败、超时、配对问题
- **音视频管道**：音频设备问题、视频捕获失败
- **配置加载**：配置项无效或冲突

诊断格式要求：
1. **问题摘要**：一句话概括发现的问题
2. **详细分析**：解释问题原因（引用具体日志行）
3. **解决建议**：给出具体可操作的建议
4. 如果日志中没有明显错误，告知用户当前运行正常

请用中文回复，语言简洁清晰。`

function loadConfig() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved) {
      const parsed = JSON.parse(saved)
      return { provider: 'openai', apiKey: '', apiBase: 'https://api.openai.com/v1', model: 'gpt-4o-mini', ...parsed }
    }
  } catch { /* ignore */ }
  return { provider: 'openai', apiKey: '', apiBase: 'https://api.openai.com/v1', model: 'gpt-4o-mini' }
}

export function useAiDiagnosis() {
  const config = reactive(loadConfig())
  const isLoading = ref(false)
  const result = ref('')
  const error = ref('')

  function saveConfig() {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ ...config }))
  }

  function onProviderChange(value) {
    const p = PROVIDERS.find((x) => x.value === value)
    if (p) {
      config.apiBase = p.base
      if (p.models.length > 0) config.model = p.models[0]
    }
  }

  function getAvailableModels() {
    const p = PROVIDERS.find((x) => x.value === config.provider)
    return p?.models || []
  }

  async function diagnose(logs) {
    if (!logs) {
      error.value = '没有可用的日志内容'
      return
    }
    if (!config.apiKey && config.provider !== 'ollama') {
      error.value = '请先配置 API Key'
      return
    }
    if (config.provider === 'custom') {
      try {
        const url = new URL(config.apiBase)
        if (!['http:', 'https:'].includes(url.protocol)) {
          throw new Error('invalid protocol')
        }
      } catch {
        error.value = '自定义提供商需要完整的 API 地址（以 http:// 或 https:// 开头）'
        return
      }
    }

    saveConfig()
    isLoading.value = true
    result.value = ''
    error.value = ''

    // Truncate logs to last 200 lines to fit token limits
    const lines = logs.split('\n')
    const truncated = lines.slice(-200).join('\n')

    try {
      const base = config.apiBase.replace(/\/+$/, '')
      const headers = { 'Content-Type': 'application/json' }
      if (config.apiKey) headers['Authorization'] = `Bearer ${config.apiKey}`

      const resp = await fetch(`${base}/chat/completions`, {
        method: 'POST',
        headers,
        body: JSON.stringify({
          model: config.model,
          messages: [
            { role: 'system', content: SYSTEM_PROMPT },
            { role: 'user', content: `请分析以下 Sunshine 日志：\n\n\`\`\`\n${truncated}\n\`\`\`` },
          ],
          temperature: 0.3,
          max_tokens: 2048,
        }),
      })

      if (!resp.ok) {
        const text = await resp.text()
        throw new Error(`API 请求失败 (${resp.status}): ${text.substring(0, 200)}`)
      }

      const data = await resp.json()
      result.value = data.choices?.[0]?.message?.content || '无法获取分析结果'
    } catch (e) {
      error.value = e.message
    } finally {
      isLoading.value = false
    }
  }

  return {
    config,
    providers: PROVIDERS,
    isLoading,
    result,
    error,
    onProviderChange,
    getAvailableModels,
    diagnose,
    saveConfig,
  }
}
