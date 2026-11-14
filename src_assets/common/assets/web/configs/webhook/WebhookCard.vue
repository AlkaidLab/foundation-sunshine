<script setup>
import { computed, onUnmounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MButton, MFieldItem, MFooter, MGroup, MItem, MLoading, MChip, MSection, MSheet, MSnackbar, MSwitchItem } from 'foundation-ui/vue'
import {
  loadWebhookConfig,
  saveWebhookConfig,
  sendWebhookTest,
} from '../../services/webhookService.js'
import {
  WEBHOOK_EVENT_IDS,
  normalizeWebhookTestRetries,
  normalizeWebhookConfigResponse,
  parseWebhookEventIds,
  serializeWebhookEventIds,
  webhookTimeoutToMilliseconds,
  webhookTimeoutToSeconds,
} from '../../utils/webhookConfig.js'

const { t } = useI18n()
const modalOpen = ref(false)
const loading = ref(false)
const loaded = ref(false)
const saving = ref(false)
const testing = ref(false)
const testRetries = ref(0)
const showWebhookUrl = ref(false)
const copiedCommand = ref('')
const errorMessage = ref('')
const successMessage = ref('')
const toastMessage = ref('')
const toastTone = ref('success')
const toastVisible = ref(false)
const loadGeneration = ref(0)
const form = ref(normalizeWebhookConfigResponse())
const savedEnabled = ref(null)
let copyResetTimer = null
let toastTimer = null
let copyGeneration = 0

const webhookEventOptions = [
  { id: 0, label: 'notifications.webhook.events.config_pin_success' },
  { id: 1, label: 'notifications.webhook.events.config_pin_failed' },
  { id: 2, label: 'notifications.webhook.events.app_launch' },
  { id: 3, label: 'notifications.webhook.events.app_resume' },
  { id: 4, label: 'notifications.webhook.events.app_terminate' },
  { id: 5, label: 'notifications.webhook.events.session_start' },
  { id: 6, label: 'notifications.webhook.events.session_end' },
]

const enabled = computed(() => form.value.webhook_enabled === 'enabled')

// The switches of the sheet; the host stores the words enabled / disabled
const enabledSwitch = computed({
  get: () => enabled.value,
  set: (value) => { form.value.webhook_enabled = value ? 'enabled' : 'disabled' },
})
const skipSslSwitch = computed({
  get: () => form.value.webhook_skip_ssl_verify === 'enabled',
  set: (value) => { form.value.webhook_skip_ssl_verify = value ? 'enabled' : 'disabled' },
})
const urlModel = computed({
  get: () => form.value.webhook_url,
  set: (value) => { form.value.webhook_url = String(value).trim() },
})
const toggleEvent = (id, on) => {
  const next = new Set(selectedEvents.value)
  if (on) next.add(id)
  else next.delete(id)
  selectedEvents.value = [...next].sort((a, b) => a - b)
}
const onSheet = (open) => { if (!open) close() }

const timeoutSeconds = computed({
  get: () => webhookTimeoutToSeconds(form.value.webhook_timeout),
  set: (value) => {
    form.value.webhook_timeout = webhookTimeoutToMilliseconds(value)
  },
})

const selectedEvents = computed({
  get: () => parseWebhookEventIds(form.value.webhook_events),
  set: (value) => {
    form.value.webhook_events = serializeWebhookEventIds(value)
  },
})

const parsedUrl = computed(() => {
  const value = form.value.webhook_url.trim()
  if (!value) return null
  try {
    return new URL(value)
  } catch {
    return null
  }
})

const usesPlainHttp = computed(() => parsedUrl.value?.protocol === 'http:')
const isUrlTooLong = (value) => new TextEncoder().encode(value).length > 4096
const hasInvalidUrlCharacters = (value) => /[\u0000-\u0020\u007f]/.test(value)

const formatLocalTimestamp = (date = new Date()) => {
  const pad = (value, width = 2) => String(value).padStart(width, '0')
  return [
    `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`,
    `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`,
  ].join(' ')
}

const testPayloadText = computed(() => {
  const prefix = 'notifications.webhook.test_payload'
  try {
    return {
      heading: t(`${prefix}.heading`),
      title: t(`${prefix}.title`),
      resultLabel: t(`${prefix}.result_label`),
      result: t(`${prefix}.result`),
      hostnameLabel: t(`${prefix}.hostname_label`),
      eventTypeLabel: t(`${prefix}.event_type_label`),
      sampleApplicationLabel: t(`${prefix}.sample_application_label`),
      sampleApplication: t(`${prefix}.sample_application`),
      sampleClientLabel: t(`${prefix}.sample_client_label`),
      sampleClient: t(`${prefix}.sample_client`),
      sampleStreamLabel: t(`${prefix}.sample_stream_label`),
      sampleStream: t(`${prefix}.sample_stream`),
      timeLabel: t(`${prefix}.time_label`),
    }
  } catch {
    return null
  }
})

const quoteShellArgument = (value, platform) => {
  if (platform === 'windows') {
    return `'${String(value).replaceAll("'", "''")}'`
  }
  return `'${String(value).replaceAll("'", "'\\''")}'`
}

const curlCommands = computed(() => {
  const url = parsedUrl.value
  if (
    !url ||
    isUrlTooLong(form.value.webhook_url.trim()) ||
    hasInvalidUrlCharacters(form.value.webhook_url.trim()) ||
    !['http:', 'https:'].includes(url.protocol) ||
    !url.hostname ||
    url.username ||
    url.password
  ) {
    return {
      linux: '',
      windows: '',
    }
  }

  const requestUrl = new URL(url.href)
  requestUrl.hash = ''
  const text = testPayloadText.value
  if (!text) {
    return {
      linux: '',
      windows: '',
    }
  }
  const payload = JSON.stringify({
    event_id: -1,
    event_type: 'webhook_test',
    msgtype: 'markdown',
    markdown: {
      content: [
        `**${text.heading}**`,
        '',
        `<font color="info">**${text.title}**</font>`,
        '',
        `>${text.resultLabel}: <font color="comment">${text.result}</font>`,
        `>${text.hostnameLabel}: <font color="comment">sunshine-host</font>`,
        `>${text.eventTypeLabel}: <font color="comment">webhook_test</font>`,
        `>${text.sampleApplicationLabel}: <font color="comment">${text.sampleApplication}</font>`,
        `>${text.sampleClientLabel}: <font color="comment">${text.sampleClient}</font>`,
        `>${text.sampleStreamLabel}: <font color="comment">${text.sampleStream}</font>`,
        `>${text.timeLabel}: <font color="comment">${formatLocalTimestamp()}</font>`,
        '',
      ].join('\n'),
    },
  })
  const buildCommand = (platform) => {
    const options = [
      platform === 'windows' ? 'curl.exe' : 'curl',
      '-X POST',
      quoteShellArgument(requestUrl.href, platform),
      `--max-time ${timeoutSeconds.value}`,
    ]
    if (url.protocol === 'https:' && form.value.webhook_skip_ssl_verify === 'enabled') {
      options.push('--insecure')
    }
    options.push(
      `-H ${quoteShellArgument('Content-Type: application/json', platform)}`,
      `--data-raw ${quoteShellArgument(payload, platform)}`,
    )
    return options.join(' ')
  }

  return {
    linux: buildCommand('linux'),
    windows: buildCommand('windows'),
  }
})

const curlTemplates = computed(() => [
  {
    id: 'linux',
    label: 'Linux / macOS',
    command: curlCommands.value.linux,
  },
  {
    id: 'windows',
    label: 'Windows PowerShell',
    command: curlCommands.value.windows,
  },
])

const clearMessages = () => {
  errorMessage.value = ''
  successMessage.value = ''
}

const dismissToast = () => {
  toastVisible.value = false
  if (toastTimer !== null) {
    clearTimeout(toastTimer)
    toastTimer = null
  }
}

const showToast = (message, tone = 'success') => {
  dismissToast()
  toastMessage.value = message
  toastTone.value = tone
  toastVisible.value = true
  toastTimer = setTimeout(() => {
    toastVisible.value = false
    toastTimer = null
  }, 5000)
}

const validateForm = ({ requireEnabledUrl = true } = {}) => {
  const value = form.value.webhook_url.trim()
  if (requireEnabledUrl && enabled.value && !value) {
    return t('config.webhook_test_url_required')
  }
  if (!value) {
    return ''
  }

  const url = parsedUrl.value
  if (
    !url ||
    isUrlTooLong(value) ||
    hasInvalidUrlCharacters(value) ||
    !['http:', 'https:'].includes(url.protocol) ||
    !url.hostname ||
    url.username ||
    url.password
  ) {
    return t('notifications.webhook.validation_url_invalid')
  }
  return ''
}

const load = async () => {
  const generation = ++loadGeneration.value
  loading.value = true
  clearMessages()
  try {
    const config = await loadWebhookConfig()
    if (generation !== loadGeneration.value) return
    form.value = normalizeWebhookConfigResponse(config)
    savedEnabled.value = form.value.webhook_enabled === 'enabled'
    loaded.value = true
  } catch (error) {
    if (generation !== loadGeneration.value) return
    loaded.value = false
    savedEnabled.value = null
    errorMessage.value = error.code === 'webhook_config_invalid'
      ? t('notifications.webhook.config_file_may_be_damaged')
      : `${t('notifications.webhook.load_failed')}: ${error.message || t('notifications.webhook.unknown_error')}`
  } finally {
    if (generation === loadGeneration.value) {
      loading.value = false
    }
  }
}

const open = async () => {
  showWebhookUrl.value = false
  modalOpen.value = true
  ++copyGeneration
  copiedCommand.value = ''
  await load()
}

const close = () => {
  if (saving.value || testing.value) return
  ++loadGeneration.value
  ++copyGeneration
  loading.value = false
  modalOpen.value = false
  clearMessages()
  showWebhookUrl.value = false
  copiedCommand.value = ''
  if (copyResetTimer !== null) {
    clearTimeout(copyResetTimer)
    copyResetTimer = null
  }
}

const save = async () => {
  if (saving.value || testing.value || loading.value) return
  clearMessages()

  const validationError = validateForm()
  if (validationError) {
    errorMessage.value = validationError
    return
  }

  const submittedForm = { ...form.value }
  const submittedEnabled = submittedForm.webhook_enabled === 'enabled'
  let savedToast = null
  saving.value = true
  try {
    const result = await saveWebhookConfig(submittedForm)
    if (result.status !== true && result.status !== 'true') {
      throw new Error(result.error || t('notifications.webhook.save_failed'))
    }
    loaded.value = true
    savedEnabled.value = submittedEnabled
    if (result.runtime_active === false || result.runtime_active === 'false') {
      savedToast = {
        message: t('notifications.webhook.save_runtime_unavailable'),
        tone: 'warning',
      }
    } else {
      savedToast = {
        message: t('notifications.webhook.save_success'),
        tone: 'success',
      }
    }
  } catch (error) {
    errorMessage.value = `${t('notifications.webhook.save_failed')}: ${error.message || t('notifications.webhook.unknown_error')}`
  } finally {
    saving.value = false
  }
  if (savedToast) {
    close()
    showToast(savedToast.message, savedToast.tone)
  }
}

const test = async () => {
  if (testing.value || saving.value || loading.value) return
  clearMessages()

  const validationError = validateForm({ requireEnabledUrl: false })
  if (validationError || !form.value.webhook_url.trim()) {
    errorMessage.value = validationError || t('config.webhook_test_url_required')
    return
  }

  testing.value = true
  try {
    testRetries.value = normalizeWebhookTestRetries(testRetries.value)
    const result = await sendWebhookTest({
      ...form.value,
      webhook_retries: testRetries.value,
    })
    if (!result.status) {
      const detail = result.http_status > 0 ? `HTTP ${result.http_status}` : result.error
      throw new Error(detail || t('notifications.webhook.unknown_error'))
    }
    successMessage.value = t('config.webhook_test_success')
  } catch (error) {
    errorMessage.value = `${t('config.webhook_test_failed')}: ${error.message || t('notifications.webhook.unknown_error')}`
  } finally {
    testing.value = false
  }
}

const selectAllEvents = () => {
  selectedEvents.value = [...WEBHOOK_EVENT_IDS]
}

const clearEvents = () => {
  selectedEvents.value = []
}

const copyCurlCommand = async (template) => {
  if (!template.command) return
  const generation = ++copyGeneration
  copiedCommand.value = ''
  if (copyResetTimer !== null) {
    clearTimeout(copyResetTimer)
    copyResetTimer = null
  }
  let copySucceeded = false
  try {
    await navigator.clipboard.writeText(template.command)
    copySucceeded = true
  } catch {
    const textArea = document.createElement('textarea')
    try {
      textArea.value = template.command
      textArea.style.position = 'fixed'
      textArea.style.opacity = '0'
      document.body.appendChild(textArea)
      textArea.select()
      copySucceeded = document.execCommand('copy')
    } catch {
      copySucceeded = false
    } finally {
      textArea.remove()
    }
  }
  if (generation !== copyGeneration || !modalOpen.value) return
  if (!copySucceeded) {
    copiedCommand.value = ''
    errorMessage.value = t('config.webhook_curl_copy_failed')
    return
  }

  copiedCommand.value = template.id
  const timer = setTimeout(() => {
    if (copyResetTimer === timer) {
      copyResetTimer = null
    }
    if (generation === copyGeneration) {
      copiedCommand.value = ''
    }
  }, 2000)
  copyResetTimer = timer
}

onUnmounted(() => {
  ++loadGeneration.value
  ++copyGeneration
  if (copyResetTimer !== null) {
    clearTimeout(copyResetTimer)
    copyResetTimer = null
  }
  if (toastTimer !== null) {
    clearTimeout(toastTimer)
    toastTimer = null
  }
})
</script>

<template>
  <section id="webhook" class="webhook-card-section">
    <MGroup>
      <MItem action icon="send" :title="$t('notifications.webhook.title')" @click="open">
        <template #trail>
          <MChip v-if="savedEnabled !== null" :tone="savedEnabled ? 'ok' : 'neutral'">{{ $t(savedEnabled ? '_common.enabled' : '_common.disabled') }}</MChip>
          <MChip v-else>{{ $t('notifications.webhook.status_unknown') }}</MChip>
        </template>
      </MItem>
    </MGroup>

    <MSheet :model-value="modalOpen" :title="$t('notifications.webhook.title')" side="end" :close-label="$t('_common.close')" :persistent="saving || testing" @update:model-value="onSheet">
      <div class="webhook-form">
        <MFooter>{{ $t('notifications.webhook.hot_apply_note') }}</MFooter>

        <div v-if="loading" class="webhook-loading" role="status"><MLoading size="28px" /><span>{{ $t('notifications.webhook.loading') }}</span></div>

        <template v-else-if="loaded">
          <MBanner v-if="errorMessage" tone="error" icon="error">{{ errorMessage }}</MBanner>
          <MBanner v-if="successMessage" tone="info" icon="check_circle">{{ successMessage }}</MBanner>

          <MGroup>
            <MSwitchItem v-model="enabledSwitch" :title="$t('config.webhook_enabled')" :info="$t('config.webhook_enabled_desc')" :disabled="saving || testing" />
          </MGroup>

          <MSection :title="$t('config.webhook_url')">
            <MGroup>
              <MFieldItem v-model="urlModel" :label="$t('config.webhook_url')" :type="showWebhookUrl ? 'url' : 'password'" :info="$t('config.webhook_url_desc')" placeholder="https://example.invalid/webhook" autocomplete="off" :disabled="saving || testing" />
              <MSwitchItem v-model="showWebhookUrl" :title="$t(showWebhookUrl ? 'notifications.webhook.hide_url' : 'notifications.webhook.show_url')" :disabled="saving || testing" />
            </MGroup>
            <MBanner v-if="usesPlainHttp" tone="warn" icon="warning">{{ $t('notifications.webhook.http_warning') }}</MBanner>
          </MSection>

          <MGroup>
            <MSwitchItem v-model="skipSslSwitch" :title="$t('config.webhook_skip_ssl_verify')" :info="$t('config.webhook_skip_ssl_verify_desc')" :disabled="saving || testing" />
            <MFieldItem v-model="timeoutSeconds" :label="$t('config.webhook_timeout')" type="number" :min="1" :max="15" :step="1" :info="$t('config.webhook_timeout_desc')" :disabled="saving || testing" />
          </MGroup>
          <MBanner v-if="skipSslSwitch" tone="warn" icon="shield">{{ $t('notifications.webhook.skip_ssl_verify_warning') }}</MBanner>

          <MSection :title="$t('config.webhook_events')">
            <div class="webhook-event-actions">
              <MButton variant="tonal" size="sm" :disabled="saving || testing" @click="selectAllEvents">{{ $t('config.webhook_events_select_all') }}</MButton>
              <MButton variant="tonal" size="sm" :disabled="saving || testing" @click="clearEvents">{{ $t('config.webhook_events_clear') }}</MButton>
            </div>
            <MGroup>
              <MSwitchItem
                v-for="event in webhookEventOptions"
                :key="event.id"
                :model-value="selectedEvents.includes(event.id)"
                :title="$t(event.label)"
                :text="`ID ${event.id}`"
                :disabled="saving || testing"
                @update:model-value="(on) => toggleEvent(event.id, on)"
              />
            </MGroup>
            <MFooter>{{ $t('config.webhook_events_desc') }}</MFooter>
          </MSection>

          <MSection :title="$t('config.webhook_test')">
            <MGroup>
              <MFieldItem v-model="testRetries" :label="$t('notifications.webhook.test_retries')" type="number" :min="0" :max="3" :step="1" :disabled="saving || testing" />
            </MGroup>
            <MButton variant="tonal" size="lg" :icon="testing ? '' : 'send'" :loading="testing" :disabled="saving || testing || !form.webhook_url.trim()" @click="test">{{ $t('config.webhook_test') }}</MButton>
          </MSection>

          <MSection :title="$t('config.webhook_curl_command')">
            <MFooter>{{ $t('config.webhook_curl_command_desc') }}</MFooter>
            <MGroup>
              <MItem v-for="template in curlTemplates" :key="template.id" icon="terminal" :title="template.label" stack>
                <template #trail>
                  <MButton variant="text" size="sm" icon="content_copy" :disabled="!template.command" @click="copyCurlCommand(template)">{{ copiedCommand === template.id ? $t('_common.success') : $t('_common.copy') }}</MButton>
                </template>
                <template #control><pre class="m-tip webhook-curl">{{ template.command }}</pre></template>
              </MItem>
            </MGroup>
          </MSection>
        </template>

        <div v-else class="webhook-load-error">
          <MBanner tone="error" icon="error">{{ errorMessage || $t('notifications.webhook.load_failed') }}</MBanner>
          <MButton variant="tonal" icon="refresh" @click="load">{{ $t('notifications.webhook.retry') }}</MButton>
        </div>
      </div>

      <template #actions>
        <MButton variant="text" :disabled="saving || testing" @click="close">{{ $t('_common.cancel') }}</MButton>
        <MButton icon="save" :loading="saving" :disabled="loading || !loaded || saving || testing" @click="save">{{ $t('_common.save') }}</MButton>
      </template>
    </MSheet>

    <MSnackbar v-model="toastVisible" :message="toastMessage" />
  </section>
</template>

<style>
.webhook-form { display: flex; flex-direction: column; gap: 16px; }
.webhook-loading { display: flex; align-items: center; gap: 12px; color: var(--md-sys-color-on-surface-variant); }
.webhook-event-actions { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 8px; }
.webhook-curl { white-space: pre-wrap; word-break: break-all; font-family: var(--md-ref-typeface-mono, ui-monospace, 'Consolas', monospace); font-size: 12.5px; }
.webhook-load-error { display: flex; flex-direction: column; align-items: flex-start; gap: 12px; }
</style>
