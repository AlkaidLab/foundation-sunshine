<script setup>
import { ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
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
              data-bs-target="#panelsStayOpen-collapseOne"
            >
              {{ $tp('config.display_device_options') }}
            </button>
          </h2>
          <div
            id="panelsStayOpen-collapseOne"
            class="accordion-collapse collapse"
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

              <!-- Device display preparation -->
              <div class="mb-3">
                <label for="display_device_prep" class="form-label">
                  {{ $tp('config.display_device_prep') }}
                </label>
                <select id="display_device_prep" class="form-select" v-model="config.display_device_prep">
                  <option value="no_operation">{{ $tp('config.display_device_prep_no_operation') }}</option>
                  <option value="ensure_active">{{ $tp('config.display_device_prep_ensure_active') }}</option>
                  <option value="ensure_primary">{{ $tp('config.display_device_prep_ensure_primary') }}</option>
                  <option value="ensure_only_display">
                    {{ $tp('config.display_device_prep_ensure_only_display') }}
                  </option>
                </select>
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
            </div>
          </div>
        </div>
      </div>
    </template>
    <template #linux> </template>
    <template #macos> </template>
  </PlatformLayout>
</template>
