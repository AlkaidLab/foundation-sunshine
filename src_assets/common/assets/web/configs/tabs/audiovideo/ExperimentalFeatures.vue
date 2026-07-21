<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'
import Checkbox from '../../../components/Checkbox.vue'

const props = defineProps({
  platform: String,
  config: Object,
  display_mode_remapping: Array,
})

const { t } = useI18n()
const config = ref(props.config)
const display_mode_remapping = ref(props.display_mode_remapping || [])

const remappingType = computed(() => {
  if (config.value.resolution_change !== 'automatic') {
    return 'refresh_rate_only'
  }
  if (config.value.refresh_rate_change !== 'automatic') {
    return 'resolution_only'
  }
  return ''
})

const filteredRemappings = computed(() => {
  const entries = []

  display_mode_remapping.value.forEach((entry, index) => {
    if (entry.type === remappingType.value) {
      entries.push({ entry, index })
    }
  })

  return entries
})

function addRemapping(type) {
  display_mode_remapping.value.push({
    type: type,
    received_resolution: '',
    received_fps: '',
    final_resolution: '',
    final_refresh_rate: '',
  })
}
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <div class="mb-3 accordion">
        <div class="accordion-item">
          <h2 class="accordion-header">
            <button
              class="accordion-button collapsed"
              type="button"
              data-bs-toggle="collapse"
              data-bs-target="#experimental-features-collapse"
            >
              {{ $t('config.experimental_features') }}
            </button>
          </h2>
          <div
            id="experimental-features-collapse"
            class="accordion-collapse collapse"
          >
            <div class="accordion-body">
              <!-- Capture Target -->
              <div class="mb-3">
                <label for="capture_target" class="form-label">{{ $t('config.capture_target') }}</label>
                <select id="capture_target" class="form-select" v-model="config.capture_target">
                  <option value="display">{{ $t('config.capture_target_display') }}</option>
                  <option value="window">{{ $t('config.capture_target_window') }}</option>
                </select>
                <div class="form-text">{{ $t('config.capture_target_desc') }}</div>
              </div>

              <!-- Window Title (only shown when capture_target is window) -->
              <div class="mb-3" v-if="config.capture_target === 'window'">
                <label for="window_title" class="form-label">{{ $t('config.window_title') }}</label>
                <input
                  type="text"
                  class="form-control"
                  id="window_title"
                  :placeholder="$t('config.window_title_placeholder')"
                  v-model="config.window_title"
                />
                <div class="form-text">{{ $t('config.window_title_desc') }}</div>
              </div>

              <!-- WGC Disable Secure Desktop -->
              <Checkbox
                class="mb-3"
                id="wgc_disable_secure_desktop"
                locale-prefix="config"
                v-model="config.wgc_disable_secure_desktop"
                default="false"
              ></Checkbox>

              <!-- Dynamic Resolution Follow Display -->
              <Checkbox
                class="mb-3"
                id="dynamic_resolution_follow_display"
                locale-prefix="config"
                v-model="config.dynamic_resolution_follow_display"
                default="true"
              ></Checkbox>

              <!-- Capture Compute Shader (HDR RGB->P010 fast path) -->
              <div class="mb-3">
                <label for="capture_compute_shader" class="form-label">
                  {{ $t('config.capture_compute_shader') }}
                </label>
                <select
                  id="capture_compute_shader"
                  class="form-select"
                  v-model="config.capture_compute_shader"
                >
                  <option value="auto">{{ $t('config.capture_compute_shader_auto') }}</option>
                  <option value="on">{{ $t('config.capture_compute_shader_on') }}</option>
                  <option value="off">{{ $t('config.capture_compute_shader_off') }}</option>
                </select>
                <div class="form-text">{{ $t('config.capture_compute_shader_desc') }}</div>
              </div>

              <!-- Display Mode Remapping -->
              <div
                class="mb-3"
                v-if="config.resolution_change === 'automatic' || config.refresh_rate_change === 'automatic'"
              >
                <label class="form-label">
                  {{ $tp('config.display_mode_remapping') }}
                </label>
                <div class="d-flex flex-column">
                  <div class="form-text">
                    <p class="pre-line">{{ $tp('config.display_mode_remapping_desc') }}</p>
                    <p v-if="remappingType === ''" class="pre-line">
                      {{ $tp('config.display_mode_remapping_default_mode_desc') }}
                    </p>
                    <p v-if="remappingType === 'resolution_only'" class="pre-line">
                      {{ $tp('config.display_mode_remapping_resolution_only_mode_desc') }}
                    </p>
                  </div>

                  <div class="remapping-table-shell" v-if="filteredRemappings.length > 0">
                  <table class="table remapping-table">
                    <thead>
                      <tr>
                        <th scope="col" v-if="remappingType !== 'refresh_rate_only'">
                          {{ $tp('config.display_mode_remapping_received_resolution') }}
                        </th>
                        <th scope="col" v-if="remappingType !== 'resolution_only'">
                          {{ $tp('config.display_mode_remapping_received_fps') }}
                        </th>
                        <th scope="col" v-if="remappingType !== 'refresh_rate_only'">
                          {{ $tp('config.display_mode_remapping_final_resolution') }}
                        </th>
                        <th scope="col" v-if="remappingType !== 'resolution_only'">
                          {{ $tp('config.display_mode_remapping_final_refresh_rate') }}
                        </th>
                        <th scope="col"></th>
                      </tr>
                    </thead>
                    <tbody>
                      <tr v-for="{ entry: c, index: i } in filteredRemappings" :key="i">
                        <template v-if="remappingType === ''">
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.received_resolution"
                              :placeholder="`1920x1080 (${$t('config.display_mode_remapping_optional')})`"
                            />
                          </td>
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.received_fps"
                              :placeholder="`60 (${$t('config.display_mode_remapping_optional')})`"
                            />
                          </td>
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.final_resolution"
                              :placeholder="`2560x1440 (${$t('config.display_mode_remapping_optional')})`"
                            />
                          </td>
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.final_refresh_rate"
                              :placeholder="`119.95 (${$t('config.display_mode_remapping_optional')})`"
                            />
                          </td>
                          <td>
                            <button
                              type="button"
                              class="remapping-delete"
                              :aria-label="$t('_common.delete')"
                              :title="$t('_common.delete')"
                              @click="display_mode_remapping.splice(i, 1)"
                            >
                              <i class="fas fa-trash"></i>
                            </button>
                          </td>
                        </template>
                        <template v-if="remappingType === 'resolution_only'">
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.received_resolution"
                              placeholder="1920x1080"
                            />
                          </td>
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.final_resolution"
                              placeholder="2560x1440"
                            />
                          </td>
                          <td>
                            <button
                              type="button"
                              class="remapping-delete"
                              :aria-label="$t('_common.delete')"
                              :title="$t('_common.delete')"
                              @click="display_mode_remapping.splice(i, 1)"
                            >
                              <i class="fas fa-trash"></i>
                            </button>
                          </td>
                        </template>
                        <template v-if="remappingType === 'refresh_rate_only'">
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.received_fps"
                              placeholder="60"
                            />
                          </td>
                          <td>
                            <input
                              type="text"
                              class="form-control monospace"
                              v-model="c.final_refresh_rate"
                              placeholder="119.95"
                            />
                          </td>
                          <td>
                            <button
                              type="button"
                              class="remapping-delete"
                              :aria-label="$t('_common.delete')"
                              :title="$t('_common.delete')"
                              @click="display_mode_remapping.splice(i, 1)"
                            >
                              <i class="fas fa-trash"></i>
                            </button>
                          </td>
                        </template>
                      </tr>
                    </tbody>
                  </table>
                  </div>
                  <button
                    type="button"
                    class="remapping-add mt-2 btn btn-primary"
                    @click="addRemapping(remappingType)"
                  >
                    &plus; Add
                  </button>
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
.pre-line {
  white-space: pre-line;
}

.remapping-table-shell {
  margin-top: 0.75rem;
  overflow-x: auto;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

.remapping-table {
  min-width: 720px;
  margin: 0;
  --bs-table-bg: transparent;
  --bs-table-color: var(--ui-text-secondary);
  --bs-table-border-color: var(--ui-border);
  --bs-table-hover-bg: var(--ui-surface-hover);
  --bs-table-hover-color: var(--ui-text-primary);
}

.remapping-table th {
  padding: 0.75rem;
  background: var(--ui-surface-strong);
  color: var(--ui-text-primary);
  font-size: 0.82rem;
  font-weight: 600;
  vertical-align: middle;
}

.remapping-table td {
  min-width: 150px;
  padding: 0.65rem;
  vertical-align: middle;
}

.remapping-table td:last-child,
.remapping-table th:last-child {
  width: 52px;
  min-width: 52px;
  text-align: center;
}

.monospace {
  font-family: 'SF Mono', 'Cascadia Code', Consolas, monospace;
}

.remapping-delete {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 2.1rem;
  height: 2.1rem;
  padding: 0;
  border: 1px solid var(--ui-danger-border);
  border-radius: var(--ui-radius-sm);
  background: transparent;
  color: var(--ui-danger-text);
  transition: background-color 0.2s ease, box-shadow 0.2s ease;
}

.remapping-delete:hover,
.remapping-delete:focus-visible {
  background: var(--ui-danger-soft);
  box-shadow: 0 0 0 3px var(--ui-danger-soft);
}

.remapping-add {
  align-self: flex-start;
}

@media (max-width: 575.98px) {
  .remapping-table {
    min-width: 640px;
  }

  .remapping-add {
    width: 100%;
  }
}
</style>
