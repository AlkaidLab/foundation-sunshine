<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps({
  platform: String,
  config: Object,
  display_mode_remapping: Array,
})

const { t } = useI18n()
const config = ref(props.config)
const display_mode_remapping = ref(props.display_mode_remapping || [])

function getRemappingType() {
  if (config.value.resolution_change !== 'automatic') {
    return 'refresh_rate_only'
  }
  if (config.value.refresh_rate_change !== 'automatic') {
    return 'resolution_only'
  }
  return ''
}

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
                    <p style="white-space: pre-line">{{ $tp('config.display_mode_remapping_desc') }}</p>
                    <p v-if="getRemappingType() === ''" style="white-space: pre-line">
                      {{ $tp('config.display_mode_remapping_default_mode_desc') }}
                    </p>
                    <p v-if="getRemappingType() === 'resolution_only'" style="white-space: pre-line">
                      {{ $tp('config.display_mode_remapping_resolution_only_mode_desc') }}
                    </p>
                  </div>

                  <table
                    class="table"
                    v-if="display_mode_remapping.filter((value) => value.type === getRemappingType()).length > 0"
                  >
                    <thead>
                      <tr>
                        <th scope="col" v-if="getRemappingType() !== 'refresh_rate_only'">
                          {{ $tp('config.display_mode_remapping_received_resolution') }}
                        </th>
                        <th scope="col" v-if="getRemappingType() !== 'resolution_only'">
                          {{ $tp('config.display_mode_remapping_received_fps') }}
                        </th>
                        <th scope="col" v-if="getRemappingType() !== 'refresh_rate_only'">
                          {{ $tp('config.display_mode_remapping_final_resolution') }}
                        </th>
                        <th scope="col" v-if="getRemappingType() !== 'resolution_only'">
                          {{ $tp('config.display_mode_remapping_final_refresh_rate') }}
                        </th>
                        <th scope="col"></th>
                      </tr>
                    </thead>
                    <tbody>
                      <tr v-for="(c, i) in display_mode_remapping" :key="i">
                        <template v-if="c.type === '' && c.type === getRemappingType()">
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
                            <button class="btn btn-danger" @click="display_mode_remapping.splice(i, 1)">
                              <i class="fas fa-trash"></i>
                            </button>
                          </td>
                        </template>
                        <template v-if="c.type === 'resolution_only' && c.type === getRemappingType()">
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
                            <button class="btn btn-danger" @click="display_mode_remapping.splice(i, 1)">
                              <i class="fas fa-trash"></i>
                            </button>
                          </td>
                        </template>
                        <template v-if="c.type === 'refresh_rate_only' && c.type === getRemappingType()">
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
                            <button class="btn btn-danger" @click="display_mode_remapping.splice(i, 1)">
                              <i class="fas fa-trash"></i>
                            </button>
                          </td>
                        </template>
                      </tr>
                    </tbody>
                  </table>
                  <button
                    class="ms-0 mt-2 btn btn-success"
                    style="margin: 0 auto"
                    @click="addRemapping(getRemappingType())"
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
