<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'
import Checkbox from '../../../components/Checkbox.vue'

const props = defineProps({
  platform: String,
  config: Object,
})

const config = ref(props.config)
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #windows>
      <div class="my-3 accordion">
        <div class="accordion-item">
          <h2 class="accordion-header">
            <button
              class="accordion-button collapsed"
              type="button"
              data-bs-toggle="collapse"
              data-bs-target="#capture-compatibility-overrides-collapse"
            >
              {{ $t('config.experimental_features') }}
            </button>
          </h2>
          <div
            id="capture-compatibility-overrides-collapse"
            class="accordion-collapse collapse"
          >
            <div class="accordion-body">
              <!-- Capture Target -->
              <div class="mb-3" v-if="config.capture === 'wgc'">
                <label for="capture_target" class="form-label">{{ $t('config.capture_target') }}</label>
                <select id="capture_target" class="form-select" v-model="config.capture_target">
                  <option value="display">{{ $t('config.capture_target_display') }}</option>
                  <option value="window">{{ $t('config.capture_target_window') }}</option>
                </select>
                <div class="form-text">{{ $t('config.capture_target_desc') }}</div>
              </div>

              <!-- Window Title (only shown when capture_target is window) -->
              <div class="mb-3" v-if="config.capture === 'wgc' && config.capture_target === 'window'">
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

              <!-- WGC compatibility -->
              <Checkbox
                v-if="config.capture === 'wgc'"
                class="mb-3"
                id="wgc_disable_secure_desktop"
                locale-prefix="config"
                v-model="config.wgc_disable_secure_desktop"
                default="false"
              ></Checkbox>

              <!-- Frame conversion strategy -->
              <div class="mb-3">
                <label for="capture_compute_shader" class="form-label">
                  {{ $t('config.capture_compute_shader') }}
                </label>
                <select
                  id="capture_compute_shader"
                  class="form-select"
                  v-model="config.capture_compute_shader"
                  aria-describedby="capture_compute_shader_desc"
                >
                  <option value="auto">{{ $t('config.capture_compute_shader_auto') }}</option>
                  <option value="on">{{ $t('config.capture_compute_shader_on') }}</option>
                  <option value="off">{{ $t('config.capture_compute_shader_off') }}</option>
                </select>
                <div id="capture_compute_shader_desc" class="form-text">
                  {{ $t('config.capture_compute_shader_desc') }}
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
