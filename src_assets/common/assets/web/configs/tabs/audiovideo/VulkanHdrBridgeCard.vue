<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { $tp } from '../../../platform-i18n'

const props = defineProps({
  config: Object,
})

const config = ref(props.config)
const createStatus = (overrides = {}) => ({
  state: 'idle',
  artifacts_installed: false,
  display_available: false,
  ...overrides,
})
const status = ref(createStatus())
const statusLoaded = ref(false)
const validating = ref(false)
let statusTimer
let statusActive = false

const enabled = computed({
  get: () => config.value.vdd_vulkan_hdr_bridge === 'enabled',
  set: (value) => {
    config.value.vdd_vulkan_hdr_bridge = value ? 'enabled' : 'disabled'
  },
})

const viewState = computed(() => {
  if (!enabled.value) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_disabled',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }
  if (!statusLoaded.value) {
    return {
      statusKey: 'config.vdd_vulkan_hdr_bridge_status_loading',
      tone: 'muted',
      canValidate: false,
      showValidateAction: false,
      showValidateHint: false,
    }
  }

  const { state, artifacts_installed: artifactsInstalled, display_available: displayAvailable } =
    status.value
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
  const canValidate = showValidateAction && displayAvailable && !validating.value
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

async function refreshStatus() {
  try {
    const response = await fetch('/api/vulkan-hdr-bridge')
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const result = await response.json()
    if (result.status?.toString() === 'true') status.value = result
  } catch (_) {
    status.value = createStatus({ state: 'unavailable' })
  } finally {
    statusLoaded.value = true
  }
}

async function validateBridge() {
  validating.value = true
  status.value = { ...status.value, state: 'validating' }
  try {
    const response = await fetch('/api/vulkan-hdr-bridge/validate', { method: 'POST' })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    status.value = await response.json()
  } catch (_) {
    status.value = { ...status.value, state: 'error' }
  } finally {
    validating.value = false
    statusLoaded.value = true
  }
}

function scheduleRefresh() {
  if (!statusActive) return
  if (statusTimer !== undefined) window.clearTimeout(statusTimer)
  const delay = status.value.state === 'enabled' ? 3000 : 10000
  statusTimer = window.setTimeout(async () => {
    if (!document.hidden) await refreshStatus()
    scheduleRefresh()
  }, delay)
}

async function handleVisibilityChange() {
  if (!statusActive || document.hidden) return
  await refreshStatus()
  scheduleRefresh()
}

onMounted(async () => {
  statusActive = true
  document.addEventListener('visibilitychange', handleVisibilityChange)
  await refreshStatus()
  scheduleRefresh()
})

onUnmounted(() => {
  statusActive = false
  if (statusTimer !== undefined) window.clearTimeout(statusTimer)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
})
</script>

<template>
  <div class="vulkan-hdr-card" :class="{ 'is-disabled': !enabled }">
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
          v-model="enabled"
          class="form-check-input"
          type="checkbox"
          role="switch"
          aria-describedby="vdd_vulkan_hdr_bridge_desc"
        />
      </div>
    </div>

    <div
      class="feature-status mt-3"
      :class="`is-${viewState.tone}`"
      role="status"
      aria-live="polite"
    >
      <span class="feature-status-dot" aria-hidden="true"></span>
      <span>{{ $tp(viewState.statusKey) }}</span>
    </div>

    <div
      v-if="viewState.showValidateAction"
      class="d-flex flex-wrap align-items-center gap-2 mt-3"
    >
      <button
        type="button"
        class="btn btn-outline-secondary btn-sm"
        :disabled="!viewState.canValidate"
        @click="validateBridge"
      >
        <span
          v-if="validating"
          class="spinner-border spinner-border-sm me-1"
          aria-hidden="true"
        ></span>
        {{
          $tp(
            validating
              ? 'config.vdd_vulkan_hdr_bridge_validating'
              : 'config.vdd_vulkan_hdr_bridge_validate',
          )
        }}
      </button>
      <small v-if="viewState.showValidateHint" class="text-body-secondary">
        {{ $tp('config.vdd_vulkan_hdr_bridge_validate_hint') }}
      </small>
    </div>
  </div>
</template>

<style scoped>
.vulkan-hdr-card {
  padding: 1rem;
  border: 1px solid var(--ui-border);
  border-left: 3px solid var(--ui-accent);
  border-radius: var(--ui-radius-md);
  background: var(--ui-accent-soft);
  transition:
    border-color 0.2s ease,
    background-color 0.2s ease,
    box-shadow 0.2s ease,
    opacity 0.2s ease;
}

.vulkan-hdr-card:hover {
  border-color: var(--ui-border-strong);
  box-shadow: var(--ui-shadow-sm);
}

.vulkan-hdr-card.is-disabled {
  border-left-color: var(--ui-text-muted);
  background: var(--ui-surface);
  opacity: 0.78;
}

.feature-status {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.875rem;
  font-weight: 600;
}

.feature-status-dot {
  width: 0.55rem;
  height: 0.55rem;
  flex: 0 0 auto;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 0 0.2rem color-mix(in srgb, currentColor 18%, transparent);
}

.feature-status.is-muted {
  color: var(--ui-text-muted);
}

.feature-status.is-ready,
.feature-status.is-info {
  color: var(--ui-accent);
}

.feature-status.is-success {
  color: var(--ui-success-text);
}

.feature-status.is-warning {
  color: var(--ui-warning-text);
}

@media (max-width: 575.98px) {
  .vulkan-hdr-card {
    padding: 0.75rem;
  }
}
</style>
