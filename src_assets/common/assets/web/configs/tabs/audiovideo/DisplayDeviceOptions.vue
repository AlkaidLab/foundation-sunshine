<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'
import Checkbox from '../../../components/Checkbox.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
const display_mode_remapping = ref(props.display_mode_remapping || [])
const createVulkanHdrStatus = (overrides = {}) => ({
  state: 'idle',
  artifacts_installed: false,
  display_available: false,
  ...overrides,
})
const vulkanHdrStatus = ref(createVulkanHdrStatus())
const vulkanHdrStatusLoaded = ref(false)
const vulkanHdrValidating = ref(false)
let vulkanHdrStatusTimer
let vulkanHdrStatusActive = false

const vulkanHdrEnabled = computed({
  get: () => config.value.vdd_vulkan_hdr_bridge === 'enabled',
  set: (enabled) => {
    config.value.vdd_vulkan_hdr_bridge = enabled ? 'enabled' : 'disabled'
  },
})

const vulkanHdrViewState = computed(() => {
  if (!vulkanHdrEnabled.value) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_disabled',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }
  if (!vulkanHdrStatusLoaded.value) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_loading',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }

  const { state, artifacts_installed: artifactsInstalled, display_available: displayAvailable } =
    vulkanHdrStatus.value
  if (!artifactsInstalled || state === 'unavailable') {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_unavailable',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }

  const showValidateAction = state !== 'enabled'
  const canValidate = showValidateAction && displayAvailable && !vulkanHdrValidating.value
  if (state === 'idle' && !displayAvailable) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_waiting_display',
      tone: 'muted',
      canValidate,
      showValidateAction,
      showValidateHint: true,
    }
  }

  const presentation = {
    idle: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_idle', tone: 'ready' },
    validating: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_validating', tone: 'info' },
    enabled: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_enabled', tone: 'success' },
    error: { statusKey: 'config.vdd_vulkan_hdr_bridge_status_error', tone: 'warning' },
  }[state] ?? { statusKey: 'config.vdd_vulkan_hdr_bridge_status_idle', tone: 'ready' }

  return { ...presentation, canValidate, showValidateAction, showValidateHint: false }
})

async function refreshVulkanHdrStatus() {
  try {
    const response = await fetch('/api/vulkan-hdr-bridge')
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const result = await response.json()
    if (result.status?.toString() === 'true') vulkanHdrStatus.value = result
  } catch (_) {
    vulkanHdrStatus.value = createVulkanHdrStatus({ state: 'unavailable' })
  } finally {
    vulkanHdrStatusLoaded.value = true
  }
}

async function validateVulkanHdrBridge() {
  vulkanHdrValidating.value = true
  vulkanHdrStatus.value = { ...vulkanHdrStatus.value, state: 'validating' }
  try {
    const response = await fetch('/api/vulkan-hdr-bridge/validate', { method: 'POST' })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    vulkanHdrStatus.value = await response.json()
  } catch (_) {
    vulkanHdrStatus.value = { ...vulkanHdrStatus.value, state: 'error' }
  } finally {
    vulkanHdrValidating.value = false
    vulkanHdrStatusLoaded.value = true
  }
}

function scheduleVulkanHdrStatusRefresh() {
  if (!vulkanHdrStatusActive) return
  if (vulkanHdrStatusTimer !== undefined) window.clearTimeout(vulkanHdrStatusTimer)
  const delay = vulkanHdrStatus.value.state === 'enabled' ? 3000 : 10000
  vulkanHdrStatusTimer = window.setTimeout(async () => {
    if (!document.hidden) await refreshVulkanHdrStatus()
    scheduleVulkanHdrStatusRefresh()
  }, delay)
}

async function handleVisibilityChange() {
  if (!vulkanHdrStatusActive || document.hidden) return
  await refreshVulkanHdrStatus()
  scheduleVulkanHdrStatusRefresh()
}

onMounted(async () => {
  if (props.platform !== 'windows') return
  vulkanHdrStatusActive = true
  document.addEventListener('visibilitychange', handleVisibilityChange)
  await refreshVulkanHdrStatus()
  scheduleVulkanHdrStatusRefresh()
})

onUnmounted(() => {
  vulkanHdrStatusActive = false
  if (vulkanHdrStatusTimer !== undefined) window.clearTimeout(vulkanHdrStatusTimer)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
})

// TODO: Sample for use in PR #2032
function getRemappingType() {
  // Assuming here that at least one setting is set to "automatic"
  if (config.value.resolution_change !== 'automatic') {
    return 'refresh_rate_only'
  }
  if (config.value.refresh_rate_change !== 'automatic') {
    return 'resolution_only'
  }
  return ''
}

function addRemapping(type) {
  let template = {
    type: type,
    received_resolution: '',
    received_fps: '',
    final_resolution: '',
    final_refresh_rate: '',
  }

  display_mode_remapping.value.push(template)
}
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <div class="mb-3 accordion">
        <div class="accordion-item">
          <h2 class="accordion-header" id="panelsStayOpen-headingOne">
            <button
              class="accordion-button"
              type="button"
              data-bs-toggle="collapse"
              data-bs-target="#panelsStayOpen-collapseOne"
            >
              {{ $tp('config.display_device_options') }}
            </button>
          </h2>
          <div
            id="panelsStayOpen-collapseOne"
            class="accordion-collapse collapse show"
            aria-labelledby="panelsStayOpen-headingOne"
          >
            <div class="accordion-body">
              <div class="mb-3">
                <label class="form-label">
                  {{ $tp('config.display_device_options_note') }}
                </label>
                <div class="form-text">
                  <p style="white-space: pre-line">{{ $tp('config.display_device_options_note_desc') }}</p>
                </div>
              </div>

              <!-- Display device preparation -->
              <div class="mb-3">
                <label for="display_device_prep" class="form-label">
                  {{ $tp('config.display_device_prep') }}
                </label>
                <select id="display_device_prep" class="form-select" v-model="config.display_device_prep">
                  <option value="no_operation">{{ $tp('config.display_device_prep_no_operation') }}</option>
                  <option value="ensure_active">{{ $tp('config.display_device_prep_ensure_active') }}</option>
                  <option value="ensure_primary">{{ $tp('config.display_device_prep_ensure_primary') }}</option>
                  <option value="ensure_secondary">{{ $tp('config.display_device_prep_ensure_secondary') }}</option>
                  <option value="ensure_only_display">
                    {{ $tp('config.display_device_prep_ensure_only_display') }}
                  </option>
                </select>
                <div class="form-text" v-if="config.display_device_prep">
                  {{ $tp('config.display_device_prep_' + config.display_device_prep + '_desc') }}
                </div>
              </div>

              <!-- Resolution change -->
              <div class="mb-3">
                <label for="resolution_change" class="form-label">
                  {{ $tp('config.resolution_change') }}
                </label>
                <select id="resolution_change" class="form-select" v-model="config.resolution_change">
                  <option value="no_operation">{{ $tp('config.resolution_change_no_operation') }}</option>
                  <option value="automatic">{{ $tp('config.resolution_change_automatic') }}</option>
                  <option value="manual">{{ $tp('config.resolution_change_manual') }}</option>
                </select>
                <div
                  class="form-text"
                  v-if="config.resolution_change === 'automatic' || config.resolution_change === 'manual'"
                >
                  {{ $tp('config.resolution_change_ogs_desc') }}
                </div>

                <!-- Manual resolution -->
                <div class="mt-2 ps-4" v-if="config.resolution_change === 'manual'">
                  <div class="form-text">
                    {{ $tp('config.resolution_change_manual_desc') }}
                  </div>
                  <input
                    type="text"
                    class="form-control"
                    id="manual_resolution"
                    placeholder="2560x1440"
                    v-model="config.manual_resolution"
                  />
                </div>
              </div>

              <!-- Refresh rate change -->
              <div class="mb-3">
                <label for="refresh_rate_change" class="form-label">
                  {{ $tp('config.refresh_rate_change') }}
                </label>
                <select id="refresh_rate_change" class="form-select" v-model="config.refresh_rate_change">
                  <option value="no_operation">{{ $tp('config.refresh_rate_change_no_operation') }}</option>
                  <option value="automatic">{{ $tp('config.refresh_rate_change_automatic') }}</option>
                  <option value="manual">{{ $tp('config.refresh_rate_change_manual_desc') }}</option>
                </select>

                <!-- Manual refresh rate -->
                <div class="mt-2 ps-4" v-if="config.refresh_rate_change === 'manual'">
                  <div class="form-text">
                    {{ $tp('config.refresh_rate_change_manual_desc') }}
                  </div>
                  <input
                    type="text"
                    class="form-control"
                    id="manual_refresh_rate"
                    placeholder="59.95"
                    v-model="config.manual_refresh_rate"
                  />
                </div>
              </div>

              <!-- HDR preparation -->
              <div class="mb-3">
                <label for="hdr_prep" class="form-label">
                  {{ $tp('config.hdr_prep') }}
                </label>
                <select id="hdr_prep" class="form-select" v-model="config.hdr_prep">
                  <option value="no_operation">{{ $tp('config.hdr_prep_no_operation') }}</option>
                  <option value="automatic">{{ $tp('config.hdr_prep_automatic') }}</option>
                </select>
              </div>

              <div class="vulkan-hdr-card mb-3" :class="{ 'is-disabled': !vulkanHdrEnabled }">
                <div class="d-flex align-items-start justify-content-between gap-3">
                  <div>
                    <label for="vdd_vulkan_hdr_bridge" class="form-label fw-semibold mb-1">
                      {{ $tp('config.vdd_vulkan_hdr_bridge') }}
                    </label>
                    <div id="vdd_vulkan_hdr_bridge_desc" class="form-text mt-0">
                      {{ $tp('config.vdd_vulkan_hdr_bridge_desc') }}
                    </div>
                  </div>
                  <div class="form-check form-switch flex-shrink-0 mt-1">
                    <input
                      id="vdd_vulkan_hdr_bridge"
                      v-model="vulkanHdrEnabled"
                      class="form-check-input"
                      type="checkbox"
                      role="switch"
                      aria-describedby="vdd_vulkan_hdr_bridge_desc"
                    />
                  </div>
                </div>

                <div
                  class="vulkan-hdr-status mt-3"
                  :class="`is-${vulkanHdrViewState.tone}`"
                  role="status"
                  aria-live="polite"
                >
                  <span class="vulkan-hdr-status-dot" aria-hidden="true"></span>
                  <span>{{ $tp(vulkanHdrViewState.statusKey) }}</span>
                </div>

                <div
                  v-if="vulkanHdrViewState.showValidateAction"
                  class="d-flex flex-wrap align-items-center gap-2 mt-3"
                >
                  <button
                    type="button"
                    class="btn btn-outline-secondary btn-sm"
                    :disabled="!vulkanHdrViewState.canValidate"
                    @click="validateVulkanHdrBridge"
                  >
                    <span
                      v-if="vulkanHdrValidating"
                      class="spinner-border spinner-border-sm me-1"
                      aria-hidden="true"
                    ></span>
                    {{
                      $tp(
                        vulkanHdrValidating
                          ? 'config.vdd_vulkan_hdr_bridge_validating'
                          : 'config.vdd_vulkan_hdr_bridge_validate',
                      )
                    }}
                  </button>
                  <small v-if="vulkanHdrViewState.showValidateHint" class="text-body-secondary">
                    {{ $tp('config.vdd_vulkan_hdr_bridge_validate_hint') }}
                  </small>
                </div>
              </div>

              <Checkbox
                class="mb-3"
                id="hdr_luminance_analysis"
                locale-prefix="config"
                v-model="config.hdr_luminance_analysis"
                default="true"
              ></Checkbox>
            </div>
          </div>
        </div>
      </div>
    </template>
    <template #linux> </template>
    <template #macos> </template>
  </PlatformLayout>
</template>

<style scoped>
.vulkan-hdr-card {
  padding: 1rem;
  border: 1px solid var(--bs-border-color);
  border-left: 3px solid var(--bs-primary);
  border-radius: var(--bs-border-radius);
  background: color-mix(in srgb, var(--bs-body-bg) 90%, var(--bs-primary) 10%);
  transition:
    border-color 0.2s ease,
    opacity 0.2s ease;
}

.vulkan-hdr-card.is-disabled {
  border-left-color: var(--bs-secondary-color);
  opacity: 0.78;
}

.vulkan-hdr-status {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.875rem;
  font-weight: 600;
}

.vulkan-hdr-status-dot {
  width: 0.55rem;
  height: 0.55rem;
  flex: 0 0 auto;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 0 0.2rem color-mix(in srgb, currentColor 18%, transparent);
}

.vulkan-hdr-status.is-muted {
  color: var(--bs-secondary-color);
}

.vulkan-hdr-status.is-ready {
  color: var(--bs-primary);
}

.vulkan-hdr-status.is-info {
  color: var(--bs-info-text-emphasis);
}

.vulkan-hdr-status.is-success {
  color: var(--bs-success-text-emphasis);
}

.vulkan-hdr-status.is-warning {
  color: var(--bs-warning-text-emphasis);
}
</style>
