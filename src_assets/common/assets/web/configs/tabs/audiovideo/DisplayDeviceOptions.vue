<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
const displayPreparationModes = [
  { value: 'no_operation', icon: 'fa-pause' },
  { value: 'ensure_active', icon: 'fa-play' },
  { value: 'ensure_primary', icon: 'fa-star' },
  { value: 'ensure_secondary', icon: 'fa-columns' },
  { value: 'ensure_only_display', icon: 'fa-desktop' },
]
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
              <fieldset class="display-prep-group">
                <legend class="form-label fw-semibold mb-2">
                  {{ $tp('config.display_device_prep') }}
                </legend>
                <div class="display-prep-options">
                  <label
                    v-for="mode in displayPreparationModes"
                    :key="mode.value"
                    class="display-prep-option"
                    :class="{ 'is-selected': config.display_device_prep === mode.value }"
                  >
                    <input
                      v-model="config.display_device_prep"
                      class="visually-hidden"
                      type="radio"
                      name="display_device_prep"
                      :value="mode.value"
                    />
                    <div class="display-prep-option-content">
                      <div
                        class="topology-preview"
                        :class="`is-${mode.value}`"
                        aria-hidden="true"
                      >
                        <div class="topology-screen topology-host">
                          <i class="fas fa-desktop"></i>
                          <span class="topology-off-mark"></span>
                        </div>
                        <span class="topology-link"></span>
                        <div class="topology-screen topology-target">
                          <i class="fas fa-desktop"></i>
                          <span class="topology-primary-badge">
                            <i class="fas fa-star"></i>
                          </span>
                        </div>
                      </div>
                      <div class="display-prep-copy">
                        <span class="display-prep-title">
                          <i class="fas me-1" :class="mode.icon" aria-hidden="true"></i>
                          {{ $tp('config.display_device_prep_' + mode.value) }}
                        </span>
                        <span class="display-prep-description">
                          {{ $tp('config.display_device_prep_' + mode.value + '_desc') }}
                        </span>
                      </div>
                    </div>
                  </label>
                </div>
              </fieldset>

              <details class="display-options-note">
                <summary>
                  <i class="fas fa-circle-info" aria-hidden="true"></i>
                  {{ $tp('config.display_device_options_note') }}
                </summary>
                <p class="pre-line">{{ $tp('config.display_device_options_note_desc') }}</p>
              </details>

              <div class="display-rule-grid">
                <!-- Resolution change -->
                <section class="display-rule-card">
                  <div class="display-rule-heading">
                    <i class="fas fa-expand-alt" aria-hidden="true"></i>
                    <span class="form-label mb-0">
                      {{ $tp('config.resolution_change') }}
                    </span>
                  </div>
                  <div class="display-rule-options">
                    <label
                      v-for="mode in ['no_operation', 'automatic', 'manual']"
                      :key="mode"
                      class="display-rule-option"
                      :class="{ 'is-selected': config.resolution_change === mode }"
                    >
                      <input
                        v-model="config.resolution_change"
                        class="form-check-input"
                        type="radio"
                        name="resolution_change"
                        :value="mode"
                      />
                      <span>{{ $tp('config.resolution_change_' + mode) }}</span>
                    </label>
                  </div>
                  <div
                    class="form-text"
                    v-if="config.resolution_change === 'automatic' || config.resolution_change === 'manual'"
                  >
                    {{ $tp('config.resolution_change_ogs_desc') }}
                  </div>

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
                </section>

                <!-- Refresh rate change -->
                <section class="display-rule-card">
                  <div class="display-rule-heading">
                    <i class="fas fa-gauge-high" aria-hidden="true"></i>
                    <span class="form-label mb-0">
                      {{ $tp('config.refresh_rate_change') }}
                    </span>
                  </div>
                  <div class="display-rule-options">
                    <label
                      v-for="mode in ['no_operation', 'automatic', 'manual']"
                      :key="mode"
                      class="display-rule-option"
                      :class="{ 'is-selected': config.refresh_rate_change === mode }"
                    >
                      <input
                        v-model="config.refresh_rate_change"
                        class="form-check-input"
                        type="radio"
                        name="refresh_rate_change"
                        :value="mode"
                      />
                      <span>{{ $tp('config.refresh_rate_change_' + mode) }}</span>
                    </label>
                  </div>

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
                </section>

                <!-- HDR preparation -->
                <section class="display-rule-card">
                  <div class="display-rule-heading">
                    <i class="fas fa-sun" aria-hidden="true"></i>
                    <span class="form-label mb-0">
                      {{ $tp('config.hdr_prep') }}
                    </span>
                  </div>
                  <div class="display-rule-options">
                    <label
                      v-for="mode in ['no_operation', 'automatic']"
                      :key="mode"
                      class="display-rule-option"
                      :class="{ 'is-selected': config.hdr_prep === mode }"
                    >
                      <input
                        v-model="config.hdr_prep"
                        class="form-check-input"
                        type="radio"
                        name="hdr_prep"
                        :value="mode"
                      />
                      <span>{{ $tp('config.hdr_prep_' + mode) }}</span>
                    </label>
                  </div>
                </section>
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
.display-prep-group {
  min-width: 0;
  margin: 0 0 1rem;
  padding: 0;
  border: 0;
}

.display-prep-options {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 17rem), 1fr));
  gap: 0.75rem;
}

.display-prep-option {
  min-width: 0;
  margin: 0;
  cursor: pointer;
}

.display-prep-option-content {
  display: grid;
  grid-template-columns: 7rem minmax(0, 1fr);
  align-items: center;
  gap: 0.85rem;
  height: 100%;
  padding: 0.85rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  transition:
    border-color 0.2s ease,
    background-color 0.2s ease,
    box-shadow 0.2s ease,
    transform 0.2s ease;
}

.display-prep-option:hover .display-prep-option-content {
  border-color: var(--ui-border-strong);
  background: var(--ui-surface-hover);
  transform: translateY(-1px);
}

.display-prep-option.is-selected .display-prep-option-content {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  box-shadow: 0 0 0 1px color-mix(in srgb, var(--ui-accent) 28%, transparent);
}

.display-prep-option input:focus-visible + .display-prep-option-content {
  outline: 2px solid var(--ui-accent);
  outline-offset: 2px;
}

.display-prep-copy {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 0.25rem;
}

.display-prep-title {
  color: var(--ui-text-primary);
  font-size: 0.9rem;
  font-weight: 650;
}

.display-prep-title i {
  color: var(--ui-accent);
}

.display-prep-description {
  color: var(--ui-text-secondary);
  font-size: 0.78rem;
  line-height: 1.4;
}

.topology-preview {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 3.5rem;
}

.topology-screen {
  position: relative;
  display: grid;
  width: 2.65rem;
  height: 1.75rem;
  place-items: center;
  border: 2px solid var(--ui-border-strong);
  border-radius: 0.3rem;
  background: var(--ui-surface-strong);
  color: var(--ui-text-muted);
  transition:
    border-color 0.2s ease,
    background-color 0.2s ease,
    color 0.2s ease,
    opacity 0.2s ease;
}

.topology-screen::after {
  position: absolute;
  bottom: -0.38rem;
  width: 1rem;
  height: 0.25rem;
  border-top: 2px solid currentColor;
  content: '';
}

.topology-screen i {
  font-size: 0.8rem;
}

.topology-link {
  width: 0.75rem;
  border-top: 2px solid var(--ui-border-strong);
}

.topology-primary-badge {
  position: absolute;
  top: -0.45rem;
  right: -0.45rem;
  display: none;
  width: 1rem;
  height: 1rem;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  background: var(--ui-accent);
  color: var(--ui-on-accent, #fff);
  font-size: 0.48rem;
  box-shadow: 0 0 0 2px var(--ui-surface);
}

.topology-off-mark {
  position: absolute;
  display: none;
  width: 2.8rem;
  border-top: 2px solid var(--ui-warning-text);
  transform: rotate(-34deg);
}

.topology-preview.is-no_operation .topology-link {
  border-top-style: dashed;
}

.topology-preview.is-ensure_active .topology-target,
.topology-preview.is-ensure_primary .topology-target,
.topology-preview.is-ensure_only_display .topology-target {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
}

.topology-preview.is-ensure_primary .topology-primary-badge {
  display: flex;
}

.topology-preview.is-ensure_secondary .topology-screen {
  border-color: var(--ui-accent);
  color: var(--ui-accent);
}

.topology-preview.is-ensure_secondary .topology-target {
  background: var(--ui-accent-soft);
}

.topology-preview.is-ensure_only_display .topology-host {
  opacity: 0.38;
}

.topology-preview.is-ensure_only_display .topology-off-mark {
  display: block;
}

.topology-preview.is-ensure_only_display .topology-link {
  border-top-style: dashed;
  opacity: 0.45;
}

.display-options-note {
  margin: 0 0 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  background: var(--ui-accent-soft);
  color: var(--ui-text-secondary);
}

.display-options-note summary {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.7rem 0.85rem;
  color: var(--ui-text-primary);
  font-size: 0.85rem;
  font-weight: 600;
  cursor: pointer;
  list-style: none;
}

.display-options-note summary::-webkit-details-marker {
  display: none;
}

.display-options-note summary i {
  color: var(--ui-accent);
}

.display-options-note summary::after {
  margin-left: auto;
  font-family: 'Font Awesome 6 Free';
  font-weight: 900;
  content: '\f078';
  transition: transform 0.2s ease;
}

.display-options-note[open] summary::after {
  transform: rotate(180deg);
}

.display-options-note p {
  margin: 0;
  padding: 0 0.85rem 0.85rem;
  font-size: 0.82rem;
  line-height: 1.5;
}

.display-rule-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 13rem), 1fr));
  gap: 0.75rem;
  margin-bottom: 1rem;
}

.display-rule-card {
  min-width: 0;
  padding: 0.85rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

.display-rule-heading {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  margin-bottom: 0.6rem;
}

.display-rule-heading i {
  width: 1rem;
  color: var(--ui-accent);
  text-align: center;
}

.display-rule-heading .form-label {
  font-size: 0.88rem;
  font-weight: 650;
}

.display-rule-options {
  display: grid;
  gap: 0.4rem;
}

.display-rule-option {
  display: flex;
  align-items: flex-start;
  gap: 0.5rem;
  margin: 0;
  padding: 0.55rem 0.65rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  color: var(--ui-text-secondary);
  font-size: 0.8rem;
  line-height: 1.35;
  cursor: pointer;
  transition:
    border-color 0.2s ease,
    background-color 0.2s ease,
    color 0.2s ease;
}

.display-rule-option:hover {
  border-color: var(--ui-border-strong);
  background: var(--ui-surface-hover);
}

.display-rule-option.is-selected {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  color: var(--ui-text-primary);
}

.display-rule-option .form-check-input {
  flex: 0 0 auto;
  margin: 0.1rem 0 0;
  cursor: pointer;
}

.nested-setting {
  padding: 0.75rem;
  border-left: 3px solid var(--ui-border-strong);
  border-radius: 0 var(--ui-radius-sm) var(--ui-radius-sm) 0;
  background: var(--ui-surface-strong);
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

  .display-options-note {
    padding: 0;
  }

  .display-prep-option-content {
    grid-template-columns: 6.5rem minmax(0, 1fr);
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
