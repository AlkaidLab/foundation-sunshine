<script setup>
import { ref } from 'vue'

const props = defineProps(['platform', 'config'])

const config = ref(props.config)
</script>

<template>
  <div id="amd-amf-encoder" class="config-page">
    <!-- AMF Usage -->
    <div class="mb-3">
      <label for="amd_usage" class="form-label">{{ $t('config.amd_usage') }}</label>
      <select id="amd_usage" class="form-select" v-model="config.amd_usage">
        <option value="">{{ $t('config.amd_driver_default') }}</option>
        <option value="transcoding">{{ $t('config.amd_usage_transcoding') }}</option>
        <option value="webcam">{{ $t('config.amd_usage_webcam') }}</option>
        <option value="lowlatency_high_quality">{{ $t('config.amd_usage_lowlatency_high_quality') }}</option>
        <option value="lowlatency">{{ $t('config.amd_usage_lowlatency') }}</option>
        <option value="ultralowlatency">{{ $t('config.amd_usage_ultralowlatency') }}</option>
      </select>
      <div class="form-text">{{ $t('config.amd_usage_desc') }}</div>
    </div>

    <!-- AMD Rate Control group options -->
    <div class="accordion mb-3">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button
            class="accordion-button collapsed"
            type="button"
            data-bs-toggle="collapse"
            data-bs-target="#panelsStayOpen-collapseOne"
          >
            {{ $t('config.amd_rc_group') }}
          </button>
        </h2>
        <div
          id="panelsStayOpen-collapseOne"
          class="accordion-collapse collapse"
          aria-labelledby="panelsStayOpen-headingOne"
        >
          <div class="accordion-body">
            <!-- AMF Rate Control -->
            <div class="mb-3">
              <label for="amd_rc" class="form-label">{{ $t('config.amd_rc') }}</label>
              <select id="amd_rc" class="form-select" v-model="config.amd_rc">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="cbr">{{ $t('config.amd_rc_cbr') }}</option>
                <option value="cqp">{{ $t('config.amd_rc_cqp') }}</option>
                <option value="vbr_latency">{{ $t('config.amd_rc_vbr_latency') }}</option>
                <option value="vbr_peak">{{ $t('config.amd_rc_vbr_peak') }}</option>
                <option value="qvbr">{{ $t('config.amd_rc_qvbr') }}</option>
                <option value="hqvbr">{{ $t('config.amd_rc_hqvbr') }}</option>
                <option value="hqcbr">{{ $t('config.amd_rc_hqcbr') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_rc_desc') }}</div>
            </div>

            <!-- AMF HRD Enforcement -->
            <div class="mb-3">
              <label for="amd_enforce_hrd" class="form-label">{{ $t('config.amd_enforce_hrd') }}</label>
              <select id="amd_enforce_hrd" class="form-select" v-model="config.amd_enforce_hrd">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_enforce_hrd_desc') }}</div>
            </div>

            <!-- AMF QVBR Quality Level -->
            <div class="mb-3" v-if="config.amd_rc === 'qvbr'">
              <label for="amd_qvbr_quality" class="form-label">
                {{ $t('config.amd_qvbr_quality') }}: {{ config.amd_qvbr_quality || 23 }}
              </label>
              <input
                type="range"
                class="form-range"
                id="amd_qvbr_quality"
                min="1"
                max="51"
                step="1"
                v-model="config.amd_qvbr_quality"
              />
              <div class="form-text">{{ $t('config.amd_qvbr_quality_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- AMF Quality group options -->
    <div class="accordion mb-3">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button
            class="accordion-button collapsed"
            type="button"
            data-bs-toggle="collapse"
            data-bs-target="#panelsStayOpen-collapseTwo"
          >
            {{ $t('config.amd_quality_group') }}
          </button>
        </h2>
        <div
          id="panelsStayOpen-collapseTwo"
          class="accordion-collapse collapse"
          aria-labelledby="panelsStayOpen-headingTwo"
        >
          <div class="accordion-body">
            <!-- AMF Quality -->
            <div class="mb-3">
              <label for="amd_quality" class="form-label">{{ $t('config.amd_quality') }}</label>
              <select id="amd_quality" class="form-select" v-model="config.amd_quality">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="speed">{{ $t('config.amd_quality_speed') }}</option>
                <option value="balanced">{{ $t('config.amd_quality_balanced') }}</option>
                <option value="quality">{{ $t('config.amd_quality_quality') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_quality_desc') }}</div>
            </div>

            <!-- AMD Preanalysis -->
            <div class="mb-3">
              <label for="amd_preanalysis" class="form-label">{{ $t('config.amd_preanalysis') }}</label>
              <select id="amd_preanalysis" class="form-select" v-model="config.amd_preanalysis">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_preanalysis_desc') }}</div>
            </div>

            <!-- AMD VBAQ -->
            <div class="mb-3">
              <label for="amd_vbaq" class="form-label">{{ $t('config.amd_vbaq') }}</label>
              <select id="amd_vbaq" class="form-select" v-model="config.amd_vbaq">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_vbaq_desc') }}</div>
            </div>

            <!-- H.264 Entropy Coding -->
            <div class="mb-3">
              <label for="amd_coder" class="form-label">{{ $t('config.amd_coder') }}</label>
              <select id="amd_coder" class="form-select" v-model="config.amd_coder">
                <option value="auto">{{ $t('config.coder_auto') }}</option>
                <option value="cabac">{{ $t('config.coder_cabac') }}</option>
                <option value="cavlc">{{ $t('config.coder_cavlc') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_coder_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Slices Per Frame -->
    <div class="mb-3" v-if="platform === 'windows'">
      <label for="amd_slices_per_frame" class="form-label">{{ $t('config.amd_slices_per_frame') }}</label>
      <select id="amd_slices_per_frame" class="form-select" v-model="config.amd_slices_per_frame">
        <option value="0">{{ $t('config.amd_slices_per_frame_auto') }}</option>
        <option value="1">1</option>
        <option value="2">2</option>
        <option value="3">3</option>
        <option value="4">4</option>
      </select>
      <div class="form-text">{{ $t('config.amd_slices_per_frame_desc') }}</div>
    </div>

    <!-- AVCodec-compatible AMF path -->
    <div class="mb-3" v-if="platform === 'windows'">
      <label for="amd_avcodec_compat" class="form-label">{{ $t('config.amd_avcodec_compat') }}</label>
      <select id="amd_avcodec_compat" class="form-select" v-model="config.amd_avcodec_compat">
        <option value="disabled">{{ $t('_common.disabled_def') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.amd_avcodec_compat_desc') }}</div>
    </div>

    <!-- AMF Advanced (driver workarounds) -->
    <div class="accordion mb-3" v-if="platform === 'windows'">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button
            class="accordion-button collapsed"
            type="button"
            data-bs-toggle="collapse"
            data-bs-target="#panelsStayOpen-collapseAmdAdvanced"
          >
            {{ $t('config.amd_advanced_group') }}
          </button>
        </h2>
        <div
          id="panelsStayOpen-collapseAmdAdvanced"
          class="accordion-collapse collapse"
        >
          <div class="accordion-body">
            <div class="form-text mb-3">{{ $t('config.amd_advanced_group_desc') }}</div>

            <button
              type="button"
              class="btn btn-sm btn-outline-secondary mb-3"
              @click="
                config.amd_high_motion_qb = '';
                config.amd_lowlatency_mode = '';
                config.amd_multi_hw_instance = '';
                config.amd_input_queue_size = '';
                config.amd_av1_latency_mode = '';
              "
            >
              {{ $t('config.amd_advanced_reset') }}
            </button>

            <!-- High Motion Quality Boost -->
            <div class="mb-3">
              <label for="amd_high_motion_qb" class="form-label">{{ $t('config.amd_high_motion_qb') }}</label>
              <select id="amd_high_motion_qb" class="form-select" v-model="config.amd_high_motion_qb">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_high_motion_qb_desc') }}</div>
            </div>

            <!-- Low Latency Mode -->
            <div class="mb-3">
              <label for="amd_lowlatency_mode" class="form-label">{{ $t('config.amd_lowlatency_mode') }}</label>
              <select id="amd_lowlatency_mode" class="form-select" v-model="config.amd_lowlatency_mode">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_lowlatency_mode_desc') }}</div>
            </div>

            <!-- Multi-HW Instance Encode -->
            <div class="mb-3">
              <label for="amd_multi_hw_instance" class="form-label">{{ $t('config.amd_multi_hw_instance') }}</label>
              <select id="amd_multi_hw_instance" class="form-select" v-model="config.amd_multi_hw_instance">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
                <option value="disabled">{{ $t('_common.disabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_multi_hw_instance_desc') }}</div>
            </div>

            <!-- Input Queue Size -->
            <div class="mb-3">
              <label for="amd_input_queue_size" class="form-label">{{ $t('config.amd_input_queue_size') }}</label>
              <input
                type="number"
                class="form-control"
                id="amd_input_queue_size"
                min="1"
                max="16"
                step="1"
                :placeholder="$t('config.amd_driver_default')"
                v-model.number="config.amd_input_queue_size"
              />
              <div class="form-text">{{ $t('config.amd_input_queue_size_desc') }}</div>
            </div>

            <!-- AV1 Encoding Latency Mode -->
            <div class="mb-3">
              <label for="amd_av1_latency_mode" class="form-label">{{ $t('config.amd_av1_latency_mode') }}</label>
              <select id="amd_av1_latency_mode" class="form-select" v-model="config.amd_av1_latency_mode">
                <option value="">{{ $t('config.amd_driver_default') }}</option>
                <option value="1">{{ $t('config.amd_av1_latency_mode_power_saving') }}</option>
                <option value="2">{{ $t('config.amd_av1_latency_mode_real_time') }}</option>
                <option value="3">{{ $t('config.amd_av1_latency_mode_lowest') }}</option>
              </select>
              <div class="form-text">{{ $t('config.amd_av1_latency_mode_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped></style>
