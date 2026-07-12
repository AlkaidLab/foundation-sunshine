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
const vulkanHdrStatus = ref({ state: 'idle', artifacts_installed: true })
const vulkanHdrValidating = ref(false)
let vulkanHdrStatusTimer

const vulkanHdrStatusKey = computed(() => {
  if (config.value.vdd_vulkan_hdr_bridge !== 'enabled') return 'config.vdd_vulkan_hdr_bridge_status_disabled'
  if (!vulkanHdrStatus.value.artifacts_installed || vulkanHdrStatus.value.state === 'unavailable') {
    return 'config.vdd_vulkan_hdr_bridge_status_unavailable'
  }
  const knownStates = ['idle', 'validating', 'enabled', 'error']
  const state = knownStates.includes(vulkanHdrStatus.value.state) ? vulkanHdrStatus.value.state : 'idle'
  return `config.vdd_vulkan_hdr_bridge_status_${state}`
})

async function refreshVulkanHdrStatus() {
  try {
    const response = await fetch('/api/vulkan-hdr-bridge')
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const result = await response.json()
    if (result.status?.toString() === 'true') vulkanHdrStatus.value = result
  } catch (_) {
    vulkanHdrStatus.value = { state: 'unavailable', artifacts_installed: false }
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
  }
}

onMounted(() => {
  if (props.platform !== 'windows') return
  refreshVulkanHdrStatus()
  vulkanHdrStatusTimer = window.setInterval(refreshVulkanHdrStatus, 3000)
})

onUnmounted(() => {
  if (vulkanHdrStatusTimer !== undefined) window.clearInterval(vulkanHdrStatusTimer)
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

              <div class="mb-3">
                <label for="vdd_vulkan_hdr_bridge" class="form-label">
                  {{ $tp('config.vdd_vulkan_hdr_bridge') }}
                </label>
                <select id="vdd_vulkan_hdr_bridge" class="form-select" v-model="config.vdd_vulkan_hdr_bridge">
                  <option value="enabled">{{ $tp('config.vdd_vulkan_hdr_bridge_automatic') }}</option>
                  <option value="disabled">{{ $tp('config.vdd_vulkan_hdr_bridge_disabled') }}</option>
                </select>
                <div class="form-text">{{ $tp('config.vdd_vulkan_hdr_bridge_desc') }}</div>
                <div
                  class="form-text fw-semibold"
                  :class="{
                    'text-success': vulkanHdrStatus.state === 'enabled',
                    'text-danger': vulkanHdrStatus.state === 'error' || vulkanHdrStatus.state === 'unavailable',
                  }"
                >
                  {{ $tp(vulkanHdrStatusKey) }}
                </div>
                <button
                  type="button"
                  class="btn btn-outline-secondary btn-sm mt-2"
                  :disabled="config.vdd_vulkan_hdr_bridge !== 'enabled' || vulkanHdrValidating"
                  @click="validateVulkanHdrBridge"
                >
                  {{ $tp('config.vdd_vulkan_hdr_bridge_validate') }}
                </button>
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
