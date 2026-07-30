<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
const hdrRuntimeStatus = ref(null)
const hdrRuntimeStatusLoaded = ref(false)
let hdrRuntimeStatusTimer
let hdrRuntimeStatusActive = false

const hdrAnalysisMode = computed({
  get: () => {
    const value = config.value.hdr_luminance_analysis
    if (value === true || value === 'true' || value === 'enabled' || value === 'on') return 'on'
    if (value === false || value === 'false' || value === 'disabled' || value === 'off') return 'off'
    return 'auto'
  },
  set: (mode) => {
    config.value.hdr_luminance_analysis = mode
  },
})

const activeHdrPipeline = computed(() => {
  const pipelines = hdrRuntimeStatus.value?.pipelines ?? []
  return pipelines.find((pipeline) => pipeline.hdr_mode !== 'sdr') ?? pipelines[0] ?? null
})

const hdrCardDisabled = computed(
  () => hdrAnalysisMode.value === 'off' && !activeHdrPipeline.value?.analysis_active,
)

const hdrRuntimeViewState = computed(() => {
  if (!hdrRuntimeStatusLoaded.value) {
    return { statusKey: 'config.hdr_runtime_status_loading', tone: 'muted' }
  }
  if (!hdrRuntimeStatus.value?.available) {
    return hdrAnalysisMode.value === 'off'
      ? { statusKey: '_common.disabled', tone: 'muted' }
      : { statusKey: 'config.hdr_runtime_status_unavailable', tone: 'muted' }
  }

  const pipeline = activeHdrPipeline.value
  if (!pipeline) {
    return hdrAnalysisMode.value === 'off'
      ? { statusKey: '_common.disabled', tone: 'muted' }
      : { statusKey: 'config.hdr_runtime_status_waiting', tone: 'ready' }
  }
  if (pipeline.hdr_mode === 'sdr') {
    return { statusKey: 'config.hdr_runtime_status_sdr', tone: 'muted' }
  }
  if (pipeline.analysis_failure_reason) {
    const expectedFallback =
      pipeline.analysis_mode === 'auto' && pipeline.analysis_failure_reason === 'encoder_unsupported'
    return {
      statusKey: 'config.hdr_runtime_status_fallback',
      tone: expectedFallback ? 'muted' : 'warning',
    }
  }
  if (!pipeline.analysis_active) {
    return hdrAnalysisMode.value === 'off'
      ? { statusKey: '_common.disabled', tone: 'muted' }
      : { statusKey: 'config.hdr_runtime_status_fallback', tone: 'warning' }
  }
  if (!pipeline.scene_metadata_active) {
    return { statusKey: 'config.hdr_runtime_status_starting', tone: 'info' }
  }
  return { statusKey: 'config.hdr_runtime_status_active', tone: 'success' }
})

const hdrRuntimeBadges = computed(() => {
  const pipeline = activeHdrPipeline.value
  if (!pipeline || pipeline.hdr_mode === 'sdr') return []

  const badges = [pipeline.hdr_mode.toUpperCase()]
  if (pipeline.analysis_active) {
    for (const format of pipeline.metadata_formats ?? []) {
      badges.push(format === 'hdr10_plus' ? 'HDR10+' : format === 'hdr_vivid' ? 'HDR Vivid' : format)
    }
  }
  return badges
})

const hdrRuntimeConversionLabel = computed(() => {
  const path = activeHdrPipeline.value?.conversion_path
  if (!path) return ''
  if (path === 'compute_shader_direct') return 'D3D11 Compute · Direct'
  if (path === 'compute_shader_scratch') return 'D3D11 Compute · Copy'
  return 'D3D11 Pixel Shader'
})

async function handleVisibilityChange() {
  if (!hdrRuntimeStatusActive || document.hidden) return
  await refreshHdrRuntimeStatus()
  scheduleHdrRuntimeStatusRefresh()
}

async function refreshHdrRuntimeStatus() {
  try {
    const response = await fetch('/api/runtime/hdr')
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    hdrRuntimeStatus.value = await response.json()
  } catch (_) {
    hdrRuntimeStatus.value = { available: false, pipelines: [] }
  } finally {
    hdrRuntimeStatusLoaded.value = true
  }
}

function scheduleHdrRuntimeStatusRefresh() {
  if (!hdrRuntimeStatusActive) return
  if (hdrRuntimeStatusTimer !== undefined) window.clearTimeout(hdrRuntimeStatusTimer)
  const delay = activeHdrPipeline.value ? 3000 : 10000
  hdrRuntimeStatusTimer = window.setTimeout(async () => {
    if (!document.hidden) await refreshHdrRuntimeStatus()
    scheduleHdrRuntimeStatusRefresh()
  }, delay)
}

onMounted(async () => {
  if (props.platform !== 'windows') return
  hdrRuntimeStatusActive = true
  document.addEventListener('visibilitychange', handleVisibilityChange)
  await refreshHdrRuntimeStatus()
  scheduleHdrRuntimeStatusRefresh()
})

onUnmounted(() => {
  hdrRuntimeStatusActive = false
  if (hdrRuntimeStatusTimer !== undefined) window.clearTimeout(hdrRuntimeStatusTimer)
  document.removeEventListener('visibilitychange', handleVisibilityChange)
})
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
                  <p class="display-options-note pre-line">{{ $tp('config.display_device_options_note_desc') }}</p>
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
                <div class="nested-setting mt-2" v-if="config.resolution_change === 'manual'">
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
                <div class="nested-setting mt-2" v-if="config.refresh_rate_change === 'manual'">
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

              <div
                class="hdr-feature-card mb-3"
                :class="{ 'is-disabled': hdrCardDisabled }"
              >
                <div class="feature-card-header d-flex align-items-start justify-content-between gap-3">
                  <div>
                    <label for="hdr_luminance_analysis" class="form-label fw-semibold mb-1">
                      {{ $t('config.hdr_luminance_analysis') }}
                    </label>
                    <div id="hdr_luminance_analysis_desc" class="form-text mt-0">
                      {{ $t('config.hdr_luminance_analysis_desc') }}
                    </div>
                  </div>
                  <select
                    id="hdr_luminance_analysis"
                    v-model="hdrAnalysisMode"
                    class="form-select feature-mode-select flex-shrink-0"
                    aria-describedby="hdr_luminance_analysis_desc"
                  >
                    <option value="auto">{{ $t('config.hdr_luminance_analysis_auto') }}</option>
                    <option value="on">{{ $t('_common.enabled') }}</option>
                    <option value="off">{{ $t('_common.disabled') }}</option>
                  </select>
                </div>

                <div
                  class="feature-status mt-3"
                  :class="`is-${hdrRuntimeViewState.tone}`"
                  role="status"
                  aria-live="polite"
                >
                  <span class="feature-status-dot" aria-hidden="true"></span>
                  <span>{{ $t(hdrRuntimeViewState.statusKey) }}</span>
                </div>

                <div v-if="hdrRuntimeBadges.length" class="d-flex flex-wrap gap-2 mt-2">
                  <span
                    v-for="badge in hdrRuntimeBadges"
                    :key="badge"
                    class="badge rounded-pill text-bg-secondary"
                  >
                    {{ badge }}
                  </span>
                </div>

                <div v-if="hdrRuntimeConversionLabel" class="form-text mt-2">
                  {{ $t('config.capture_compute_shader') }}: {{ hdrRuntimeConversionLabel }}
                </div>
              </div>
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
.display-options-note {
  margin: 0;
  padding: 0.75rem 0.9rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-accent-soft);
  color: var(--ui-text-secondary);
}

.nested-setting {
  margin-left: 0.75rem;
  padding: 0.75rem;
  border-left: 3px solid var(--ui-border-strong);
  border-radius: 0 var(--ui-radius-sm) var(--ui-radius-sm) 0;
  background: var(--ui-surface);
}

.hdr-feature-card {
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

.hdr-feature-card:hover {
  border-color: var(--ui-border-strong);
  box-shadow: var(--ui-shadow-sm);
}

.hdr-feature-card.is-disabled {
  border-left-color: var(--ui-text-muted);
  background: var(--ui-surface);
  opacity: 0.78;
}

.feature-mode-select {
  width: min(13rem, 42%);
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

.feature-status.is-ready {
  color: var(--ui-accent);
}

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
  .display-options-note,
  .nested-setting,
  .hdr-feature-card {
    padding: 0.75rem;
  }

  .nested-setting {
    margin-left: 0;
  }

  .feature-mode-select {
    width: 100%;
  }

  .feature-card-header {
    flex-direction: column;
    align-items: stretch !important;
  }
}
</style>
