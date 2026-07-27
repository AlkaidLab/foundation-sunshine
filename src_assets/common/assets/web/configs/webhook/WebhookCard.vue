<script setup>
import { computed, nextTick, onUnmounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
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
const modalRef = ref(null)
const cardRef = ref(null)
const lastFocusedElement = ref(null)
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
  lastFocusedElement.value = document.activeElement
  showWebhookUrl.value = false
  modalOpen.value = true
  ++copyGeneration
  copiedCommand.value = ''
  await nextTick()
  modalRef.value?.focus()
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
  const target = lastFocusedElement.value || cardRef.value
  lastFocusedElement.value = null
  void nextTick(() => {
    if (target && document.contains(target)) {
      target.focus?.()
    }
  })
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

const getFocusableElements = () => {
  if (!modalRef.value) return []
  return Array.from(
    modalRef.value.querySelectorAll(
      [
        'button:not([disabled])',
        '[href]',
        'input:not([disabled])',
        'select:not([disabled])',
        'textarea:not([disabled])',
        'summary',
        '[tabindex]:not([tabindex="-1"])',
      ].join(','),
    ),
  ).filter((element) => element.offsetParent !== null)
}

const trapFocus = (event) => {
  const focusable = getFocusableElements()
  if (!focusable.length) {
    event.preventDefault()
    modalRef.value?.focus()
    return
  }

  const currentIndex = focusable.indexOf(document.activeElement)
  const lastIndex = focusable.length - 1
  let nextIndex = currentIndex + 1
  if (event.shiftKey) {
    nextIndex = currentIndex <= 0 ? lastIndex : currentIndex - 1
  } else if (currentIndex === -1 || currentIndex >= lastIndex) {
    nextIndex = 0
  }
  event.preventDefault()
  focusable[nextIndex].focus()
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
    <button ref="cardRef" type="button" class="notification-card" @click="open">
      <span class="notification-card-icon">
        <i class="fas fa-paper-plane"></i>
      </span>
      <span class="notification-card-content">
        <span class="notification-card-title">{{ $t('notifications.webhook.title') }}</span>
        <span class="notification-card-description">{{ $t('notifications.webhook.description') }}</span>
        <span class="notification-card-state">
          <template v-if="savedEnabled !== null">
            <span class="status-dot" :class="{ enabled: savedEnabled }"></span>
            {{ $t(savedEnabled ? '_common.enabled' : '_common.disabled') }}
          </template>
          <template v-else>
            {{ $t('notifications.webhook.status_unknown') }}
          </template>
        </span>
      </span>
      <span class="notification-card-action">
        {{ $t('notifications.webhook.configure') }}
        <i class="fas fa-chevron-right"></i>
      </span>
    </button>

    <Teleport to="body">
      <Transition name="notification-modal">
        <div v-if="modalOpen" class="notification-modal-overlay">
          <div
            ref="modalRef"
            class="notification-modal"
            role="dialog"
            aria-modal="true"
            aria-labelledby="webhook-modal-title"
            tabindex="-1"
            @keydown.esc.prevent.stop
            @keydown.tab="trapFocus"
          >
            <div class="notification-modal-header">
              <div>
                <h5 id="webhook-modal-title">
                  <i class="fas fa-paper-plane me-2"></i>
                  {{ $t('notifications.webhook.title') }}
                </h5>
                <p>{{ $t('notifications.webhook.hot_apply_note') }}</p>
              </div>
            </div>

            <div class="notification-modal-body">
              <div v-if="loading" class="notification-loading" role="status">
                <i class="fas fa-spinner fa-spin"></i>
                <span>{{ $t('notifications.webhook.loading') }}</span>
              </div>

              <template v-else-if="loaded">
                <div v-if="errorMessage" class="alert alert-danger" role="alert">{{ errorMessage }}</div>
                <div v-if="successMessage" class="alert alert-success" role="status">{{ successMessage }}</div>

                <div class="mb-3 webhook-switch-setting">
                  <div class="form-check form-switch">
                    <input
                      id="webhook_enabled"
                      v-model="form.webhook_enabled"
                      class="form-check-input"
                      type="checkbox"
                      true-value="enabled"
                      false-value="disabled"
                      :disabled="saving || testing"
                    />
                    <label for="webhook_enabled" class="form-check-label">
                      {{ $t('config.webhook_enabled') }}
                    </label>
                  </div>
                  <div class="form-text">{{ $t('config.webhook_enabled_desc') }}</div>
                </div>

                <div class="mb-3">
                  <label for="webhook_url" class="form-label">{{ $t('config.webhook_url') }}</label>
                  <div class="input-group">
                    <input
                      id="webhook_url"
                      v-model.trim="form.webhook_url"
                      :type="showWebhookUrl ? 'url' : 'password'"
                      class="form-control"
                      maxlength="4096"
                      placeholder="https://example.invalid/webhook"
                      autocomplete="off"
                      spellcheck="false"
                      :disabled="saving || testing"
                    />
                    <button
                      type="button"
                      class="btn btn-outline-secondary webhook-url-visibility"
                      :disabled="saving || testing"
                      :aria-label="$t(showWebhookUrl ? 'notifications.webhook.hide_url' : 'notifications.webhook.show_url')"
                      :title="$t(showWebhookUrl ? 'notifications.webhook.hide_url' : 'notifications.webhook.show_url')"
                      :aria-pressed="showWebhookUrl"
                      @click="showWebhookUrl = !showWebhookUrl"
                    >
                      <i :class="showWebhookUrl ? 'fas fa-eye-slash' : 'fas fa-eye'" aria-hidden="true"></i>
                    </button>
                  </div>
                  <div class="form-text">{{ $t('config.webhook_url_desc') }}</div>
                  <div v-if="usesPlainHttp" class="alert alert-warning mt-2 mb-0" role="alert">
                    <i class="fas fa-exclamation-triangle me-2"></i>
                    {{ $t('notifications.webhook.http_warning') }}
                  </div>
                </div>

                <div class="mb-3 webhook-switch-setting">
                  <div class="form-check form-switch">
                    <input
                      id="webhook_skip_ssl_verify"
                      v-model="form.webhook_skip_ssl_verify"
                      class="form-check-input"
                      type="checkbox"
                      true-value="enabled"
                      false-value="disabled"
                      :disabled="saving || testing"
                    />
                    <label for="webhook_skip_ssl_verify" class="form-check-label">
                      {{ $t('config.webhook_skip_ssl_verify') }}
                    </label>
                  </div>
                  <div class="form-text">{{ $t('config.webhook_skip_ssl_verify_desc') }}</div>
                  <div
                    v-if="form.webhook_skip_ssl_verify === 'enabled'"
                    class="alert alert-warning mt-2 mb-0"
                    role="alert"
                  >
                    <i class="fas fa-shield-alt me-2"></i>
                    {{ $t('notifications.webhook.skip_ssl_verify_warning') }}
                  </div>
                </div>

                <div class="mb-3">
                  <label for="webhook_timeout" class="form-label">{{ $t('config.webhook_timeout') }}</label>
                  <input
                    id="webhook_timeout"
                    v-model.number="timeoutSeconds"
                    type="number"
                    min="1"
                    max="15"
                    step="1"
                    class="form-control"
                    :disabled="saving || testing"
                  />
                  <div class="form-text">{{ $t('config.webhook_timeout_desc') }}</div>
                </div>

                <fieldset class="mb-3">
                  <legend class="form-label fs-6">{{ $t('config.webhook_events') }}</legend>
                  <div class="d-flex flex-wrap gap-2 mb-3">
                    <button
                      type="button"
                      class="btn btn-sm btn-outline-secondary"
                      :disabled="saving || testing"
                      @click="selectAllEvents"
                    >
                      {{ $t('config.webhook_events_select_all') }}
                    </button>
                    <button
                      type="button"
                      class="btn btn-sm btn-outline-secondary"
                      :disabled="saving || testing"
                      @click="clearEvents"
                    >
                      {{ $t('config.webhook_events_clear') }}
                    </button>
                  </div>
                  <div class="row g-2">
                    <div v-for="event in webhookEventOptions" :key="event.id" class="col-sm-6">
                      <div class="form-check notification-event">
                        <input
                          :id="`webhook_event_${event.id}`"
                          v-model="selectedEvents"
                          class="form-check-input"
                          type="checkbox"
                          :value="event.id"
                          :disabled="saving || testing"
                        />
                        <label class="form-check-label" :for="`webhook_event_${event.id}`">
                          <span class="event-id">{{ event.id }}</span>
                          {{ $t(event.label) }}
                        </label>
                      </div>
                    </div>
                  </div>
                  <div class="form-text mt-2">{{ $t('config.webhook_events_desc') }}</div>
                </fieldset>

                <div class="webhook-tools">
                  <div class="webhook-test-controls">
                    <div>
                      <label for="webhook_test_retries" class="form-label">
                        {{ $t('notifications.webhook.test_retries') }}
                      </label>
                      <input
                        id="webhook_test_retries"
                        v-model.number="testRetries"
                        type="number"
                        min="0"
                        max="3"
                        step="1"
                        class="form-control"
                        :disabled="saving || testing"
                      />
                    </div>
                    <button
                      type="button"
                      class="btn btn-outline-info"
                      :disabled="saving || testing || !form.webhook_url.trim()"
                      @click="test"
                    >
                      <i :class="testing ? 'fas fa-spinner fa-spin me-1' : 'fas fa-paper-plane me-1'"></i>
                      {{ $t('config.webhook_test') }}
                    </button>
                  </div>

                  <details class="curl-details" :class="{ disabled: !curlCommands.linux }">
                    <summary>
                      <i class="fas fa-terminal me-1"></i>
                      {{ $t('config.webhook_curl_command') }}
                    </summary>
                    <p>{{ $t('config.webhook_curl_command_desc') }}</p>
                    <div
                      v-for="template in curlTemplates"
                      :key="template.id"
                      class="curl-template"
                    >
                      <strong>{{ template.label }}</strong>
                      <pre>{{ template.command }}</pre>
                      <button
                        type="button"
                        class="btn btn-sm btn-outline-secondary"
                        :disabled="!template.command"
                        @click="copyCurlCommand(template)"
                      >
                        <i class="fas fa-copy me-1"></i>
                        {{ copiedCommand === template.id ? $t('_common.success') : $t('_common.copy') }}
                      </button>
                    </div>
                  </details>
                </div>
              </template>

              <div v-else class="notification-load-error">
                <i class="fas fa-exclamation-circle"></i>
                <p>{{ errorMessage || $t('notifications.webhook.load_failed') }}</p>
                <button type="button" class="btn btn-outline-primary" @click="load">
                  {{ $t('notifications.webhook.retry') }}
                </button>
              </div>
            </div>

            <div class="notification-modal-footer">
              <button type="button" class="btn btn-secondary" :disabled="saving || testing" @click="close">
                {{ $t('_common.cancel') }}
              </button>
              <button
                type="button"
                class="btn btn-primary"
                :disabled="loading || !loaded || saving || testing"
                @click="save"
              >
                <i v-if="saving" class="fas fa-spinner fa-spin me-1"></i>
                <i v-else class="fas fa-save me-1"></i>
                {{ $t('_common.save') }}
              </button>
            </div>
          </div>
        </div>
      </Transition>
      <Transition name="webhook-toast">
        <div v-if="toastVisible" class="webhook-toast-container">
          <div
            class="toast align-items-center border-0 show"
            :class="toastTone === 'warning' ? 'text-bg-warning' : 'text-bg-success'"
            role="alert"
            aria-live="assertive"
            aria-atomic="true"
          >
            <div class="d-flex">
              <div class="toast-body">
                <i
                  class="me-2"
                  :class="toastTone === 'warning' ? 'fas fa-exclamation-triangle' : 'fas fa-check-circle'"
                ></i>
                {{ toastMessage }}
              </div>
              <button
                type="button"
                class="btn-close"
                :class="{ 'btn-close-white': toastTone !== 'warning' }"
                :aria-label="$t('_common.close')"
                @click="dismissToast"
              ></button>
            </div>
          </div>
        </div>
      </Transition>
    </Teleport>
  </section>
</template>

<style scoped lang="less">
.webhook-card-section {
  margin-top: 1.5rem;
}

.notification-card {
  width: 100%;
  min-height: 170px;
  display: grid;
  grid-template-columns: auto 1fr auto;
  align-items: center;
  gap: 1.25rem;
  padding: 1.5rem;
  border: 1px solid var(--bs-border-color);
  border-radius: 1.25rem;
  color: var(--bs-body-color);
  background:
    radial-gradient(circle at 12% 20%, rgba(13, 110, 253, 0.16), transparent 34%),
    var(--bs-body-bg);
  text-align: left;
  box-shadow: 0 12px 32px rgba(0, 0, 0, 0.08);
  transition: transform 160ms ease, border-color 160ms ease, box-shadow 160ms ease;

  &:hover,
  &:focus-visible {
    transform: translateY(-2px);
    border-color: rgba(13, 110, 253, 0.65);
    box-shadow: 0 16px 38px rgba(13, 110, 253, 0.14);
    outline: none;
  }
}

.notification-card-icon {
  width: 70px;
  height: 70px;
  display: grid;
  place-items: center;
  border-radius: 1.25rem;
  color: #fff;
  background: linear-gradient(135deg, #0d6efd, #6f42c1);
  font-size: 1.75rem;
  box-shadow: 0 10px 24px rgba(13, 110, 253, 0.28);
}

.notification-card-content {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 0.35rem;
}

.notification-card-title {
  font-size: 1.25rem;
  font-weight: 700;
}

.notification-card-description,
.notification-card-state {
  color: var(--bs-secondary-color);
}

.notification-card-state {
  display: inline-flex;
  align-items: center;
  gap: 0.45rem;
  margin-top: 0.3rem;
  font-size: 0.9rem;
}

.status-dot {
  width: 0.65rem;
  height: 0.65rem;
  border-radius: 50%;
  background: var(--bs-secondary);

  &.enabled {
    background: var(--bs-success);
    box-shadow: 0 0 0 4px rgba(25, 135, 84, 0.14);
  }
}

.notification-card-action {
  display: inline-flex;
  align-items: center;
  gap: 0.65rem;
  color: var(--bs-primary);
  font-weight: 600;
}

.notification-modal-overlay {
  position: fixed;
  inset: 0;
  z-index: 10020;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 1.25rem;
  background: rgba(0, 0, 0, 0.68);
  backdrop-filter: blur(8px);
}

.webhook-toast-container {
  position: fixed;
  top: 1.5rem;
  left: 50%;
  z-index: 10030;
  width: min(420px, calc(100vw - 3rem));
  transform: translateX(-50%);

  .toast {
    width: 100%;
    max-width: none;
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.24);
  }

  .btn-close {
    margin: auto 0.75rem auto 0;
  }
}

.notification-modal {
  width: min(820px, 100%);
  max-height: calc(100vh - 2.5rem);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border: 1px solid var(--bs-border-color);
  border-radius: 1.25rem;
  color: var(--ui-text-primary);
  background: var(--ui-surface-strong);
  box-shadow: 0 28px 70px rgba(0, 0, 0, 0.34);
}

.notification-modal-header,
.notification-modal-footer {
  flex: 0 0 auto;
  padding: 1.15rem 1.35rem;
}

.notification-modal-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
  border-bottom: 1px solid var(--bs-border-color);

  h5 {
    margin: 0;
    color: var(--ui-text-primary);
  }

  p {
    margin: 0.35rem 0 0;
    color: var(--ui-text-secondary);
    font-size: 0.9rem;
  }
}

.notification-modal-body {
  flex: 1 1 auto;
  overflow-y: auto;
  padding: 1.35rem;
  overscroll-behavior: contain;
  touch-action: pan-y;
  -webkit-overflow-scrolling: touch;
}

.notification-modal {
  .form-label,
  .form-check-label,
  legend {
    color: var(--ui-text-primary);
    font-weight: 600;
  }

  .form-text,
  .curl-details p {
    color: var(--ui-text-secondary);
  }

  .form-control {
    color: var(--ui-text-primary);
  }
}

[data-bs-theme='light'] .notification-modal {
  background: #f8fbff;

  .form-text,
  .curl-details p,
  .notification-loading,
  .notification-load-error {
    color: #475569;
  }
}

.webhook-switch-setting {
  .form-check {
    min-height: 1.75rem;
  }

  .form-check-input {
    width: 2.5rem;
    height: 1.25rem;
    margin-right: 0.65rem;
  }
}

.webhook-url-visibility {
  min-width: 2.75rem;
  border-color: var(--ui-border);
  color: var(--ui-text-primary);

  &:hover,
  &:focus-visible {
    border-color: var(--ui-border-strong);
    background: var(--ui-accent-soft);
    color: var(--ui-accent-text);
  }
}

.notification-modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 0.75rem;
  border-top: 1px solid var(--bs-border-color);
}

.notification-loading,
.notification-load-error {
  min-height: 260px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-direction: column;
  gap: 0.85rem;
  color: var(--bs-secondary-color);
  text-align: center;

  i {
    font-size: 1.75rem;
  }
}

.notification-event {
  height: 100%;
  padding: 0.75rem 0.75rem 0.75rem 2.25rem;
  border: 1px solid var(--bs-border-color);
  border-radius: 0.75rem;
}

.event-id {
  display: inline-grid;
  min-width: 1.55rem;
  height: 1.55rem;
  place-items: center;
  margin-right: 0.35rem;
  border-radius: 0.45rem;
  color: var(--bs-primary);
  background: rgba(13, 110, 253, 0.12);
  font-size: 0.78rem;
  font-weight: 700;
}

.webhook-tools {
  display: grid;
  gap: 0.9rem;
  margin-top: 1.25rem;
}

.webhook-test-controls {
  display: grid;
  grid-template-columns: minmax(8rem, 12rem) auto;
  align-items: end;
  gap: 0.75rem;

  .btn {
    justify-self: start;
  }
}

.curl-details {
  padding: 0.9rem;
  border: 1px solid var(--bs-border-color);
  border-radius: 0.75rem;

  &.disabled {
    opacity: 0.55;
    pointer-events: none;
  }

  summary {
    cursor: pointer;
    color: var(--bs-primary);
    font-weight: 600;
  }

  p {
    margin: 0.75rem 0;
    color: var(--bs-secondary-color);
  }

  pre {
    max-height: 220px;
    overflow: auto;
    padding: 0.85rem;
    border-radius: 0.6rem;
    background: var(--bs-tertiary-bg);
    white-space: pre-wrap;
    word-break: break-all;
  }

  .curl-template + .curl-template {
    margin-top: 1rem;
  }

  .curl-template strong {
    display: block;
    margin-bottom: 0.4rem;
  }
}

.notification-modal-enter-active,
.notification-modal-leave-active {
  transition: opacity 160ms ease;

  .notification-modal {
    transition: transform 160ms ease, opacity 160ms ease;
  }
}

.notification-modal-enter-from,
.notification-modal-leave-to {
  opacity: 0;

  .notification-modal {
    opacity: 0;
    transform: translateY(12px) scale(0.985);
  }
}

.webhook-toast-enter-active,
.webhook-toast-leave-active {
  transition: opacity 160ms ease, transform 160ms ease;
}

.webhook-toast-enter-from,
.webhook-toast-leave-to {
  opacity: 0;
  transform: translate(-50%, -10px);
}

@media (max-width: 575.98px) {
  .webhook-test-controls {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 640px) {
  .notification-card {
    grid-template-columns: auto 1fr;
  }

  .notification-card-action {
    grid-column: 1 / -1;
    justify-content: flex-end;
  }

  .notification-modal-overlay {
    padding: 0;
  }

  .notification-modal {
    width: 100%;
    max-height: 100vh;
    min-height: 100vh;
    border-radius: 0;
  }

  .webhook-toast-container {
    top: 1rem;
    width: calc(100vw - 2rem);
  }
}
</style>
