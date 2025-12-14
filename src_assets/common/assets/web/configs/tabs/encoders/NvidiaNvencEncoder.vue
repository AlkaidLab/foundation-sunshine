<script setup>
import { ref } from 'vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="nvidia-nvenc-encoder" class="config-page">
    <!-- Performance preset -->
    <div class="mb-3">
      <label for="nvenc_preset" class="form-label">{{ $t('config.nvenc_preset') }}</label>
      <select id="nvenc_preset" class="form-select" v-model="config.nvenc_preset">
        <option value="1">P1 {{ $t('config.nvenc_preset_1') }}</option>
        <option value="2">P2</option>
        <option value="3">P3</option>
        <option value="4">P4</option>
        <option value="5">P5</option>
        <option value="6">P6</option>
        <option value="7">P7 {{ $t('config.nvenc_preset_7') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_preset_desc') }}</div>
    </div>

    <!-- Two-pass mode -->
    <div class="mb-3">
      <label for="nvenc_twopass" class="form-label">{{ $t('config.nvenc_twopass') }}</label>
      <select id="nvenc_twopass" class="form-select" v-model="config.nvenc_twopass">
        <option value="disabled">{{ $t('config.nvenc_twopass_disabled') }}</option>
        <option value="quarter_res">{{ $t('config.nvenc_twopass_quarter_res') }}</option>
        <option value="full_res">{{ $t('config.nvenc_twopass_full_res') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_twopass_desc') }}</div>
    </div>

    <!-- Spatial AQ -->
    <div class="mb-3">
      <label for="nvenc_spatial_aq" class="form-label">{{ $t('config.nvenc_spatial_aq') }}</label>
      <select id="nvenc_spatial_aq" class="form-select" v-model="config.nvenc_spatial_aq">
        <option value="disabled">{{ $t('config.nvenc_spatial_aq_disabled') }}</option>
        <option value="enabled">{{ $t('config.nvenc_spatial_aq_enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_spatial_aq_desc') }}</div>
    </div>

    <!-- Temporal AQ -->
    <!-- <div class="mb-3">
      <label for="nvenc_temporal_aq" class="form-label">{{ $t('config.nvenc_temporal_aq') }}</label>
      <select id="nvenc_temporal_aq" class="form-select" v-model="config.nvenc_temporal_aq">
        <option value="disabled">{{ $t('_common.disabled_def') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_temporal_aq_desc') }}</div>
    </div> -->

    <!-- Lookahead depth -->
    <!-- <div class="mb-3">
      <label for="nvenc_lookahead_depth" class="form-label">{{ $t('config.nvenc_lookahead_depth') }}</label>
      <input type="number" min="0" max="32" class="form-control" id="nvenc_lookahead_depth" placeholder="0"
             v-model.number="config.nvenc_lookahead_depth" />
      <div class="form-text">{{ $t('config.nvenc_lookahead_depth_desc') }}</div>
    </div> -->

    <!-- Lookahead level -->
    <!-- <div class="mb-3" v-if="config.nvenc_lookahead_depth > 0">
      <label for="nvenc_lookahead_level" class="form-label">{{ $t('config.nvenc_lookahead_level') }}</label>
      <select id="nvenc_lookahead_level" class="form-select" v-model="config.nvenc_lookahead_level">
        <option value="disabled">{{ $t('config.nvenc_lookahead_level_disabled') }}</option>
        <option value="0">{{ $t('config.nvenc_lookahead_level_0') }}</option>
        <option value="1">{{ $t('config.nvenc_lookahead_level_1') }}</option>
        <option value="2">{{ $t('config.nvenc_lookahead_level_2') }}</option>
        <option value="3">{{ $t('config.nvenc_lookahead_level_3') }}</option>
        <option value="autoselect">{{ $t('config.nvenc_lookahead_level_autoselect') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_lookahead_level_desc') }}</div>
    </div> -->

    <!-- Temporal filter -->
    <!-- <div class="mb-3">
      <label for="nvenc_temporal_filter" class="form-label">{{ $t('config.nvenc_temporal_filter') }}</label>
      <select id="nvenc_temporal_filter" class="form-select" v-model="config.nvenc_temporal_filter">
        <option value="disabled">{{ $t('_common.disabled_def') }}</option>
        <option value="0">{{ $t('config.nvenc_temporal_filter_disabled') }}</option>
        <option value="4">{{ $t('config.nvenc_temporal_filter_4') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_temporal_filter_desc') }}</div>
    </div> -->

    <!-- Rate control mode -->
    <div class="mb-3">
      <label for="nvenc_rate_control" class="form-label">{{ $t('config.nvenc_rate_control') }}</label>
      <select id="nvenc_rate_control" class="form-select" v-model="config.nvenc_rate_control">
        <option value="cbr">{{ $t('config.nvenc_rate_control_cbr') }}</option>
        <option value="vbr">{{ $t('config.nvenc_rate_control_vbr') }}</option>
      </select>
      <div class="form-text">{{ $t('config.nvenc_rate_control_desc') }}</div>
    </div>

    <!-- Target quality (VBR mode only) -->
    <div class="mb-3" v-if="config.nvenc_rate_control === 'vbr'">
      <label for="nvenc_target_quality" class="form-label">{{ $t('config.nvenc_target_quality') }}</label>
      <input type="number" min="0" max="63" class="form-control" id="nvenc_target_quality" placeholder="0"
             v-model.number="config.nvenc_target_quality" />
      <div class="form-text">{{ $t('config.nvenc_target_quality_desc') }}</div>
    </div>

    <!-- Single-frame VBV/HRD percentage increase -->
    <div class="mb-3">
      <label for="nvenc_vbv_increase" class="form-label">{{ $t('config.nvenc_vbv_increase') }}</label>
      <input type="number" min="0" max="400" class="form-control" id="nvenc_vbv_increase" placeholder="0"
             v-model="config.nvenc_vbv_increase" />
      <div class="form-text">
        {{ $t('config.nvenc_vbv_increase_desc') }}<br>
        <br>
        <a href="https://en.wikipedia.org/wiki/Video_buffering_verifier">VBV/HRD</a>
      </div>
    </div>

    <!-- Miscellaneous options -->
    <div class="accordion">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.misc') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <!-- NVENC Realtime HAGS priority -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_realtime_hags" class="form-label">{{ $t('config.nvenc_realtime_hags') }}</label>
              <select id="nvenc_realtime_hags" class="form-select" v-model="config.nvenc_realtime_hags">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">
                {{ $t('config.nvenc_realtime_hags_desc') }}<br>
                <br>
                <a href="https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/">HAGS</a>
              </div>
            </div>

            <!-- Split frame encoding -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_split_encode" class="form-label">{{ $t('config.nvenc_split_encode') }}</label>
              <select id="nvenc_split_encode" class="form-select" v-model="config.nvenc_split_encode">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="driver_decides">{{ $t('config.nvenc_split_encode_driver_decides_def') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
                <option value="two_strips">{{ $t('config.nvenc_split_encode_two_strips') }}</option>
                <option value="three_strips">{{ $t('config.nvenc_split_encode_three_strips') }}</option>
                <option value="four_strips">{{ $t('config.nvenc_split_encode_four_strips') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_split_encode_desc') }}</div>
            </div>

            <!-- Prefer lower encoding latency over power savings -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_latency_over_power" class="form-label">{{ $t('config.nvenc_latency_over_power') }}</label>
              <select id="nvenc_latency_over_power" class="form-select" v-model="config.nvenc_latency_over_power">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_latency_over_power_desc') }}</div>
            </div>

            <!-- Present OpenGL/Vulkan on top of DXGI -->
            <div class="mb-3" v-if="platform === 'windows'">
              <label for="nvenc_opengl_vulkan_on_dxgi" class="form-label">{{ $t('config.nvenc_opengl_vulkan_on_dxgi') }}</label>
              <select id="nvenc_opengl_vulkan_on_dxgi" class="form-select" v-model="config.nvenc_opengl_vulkan_on_dxgi">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_opengl_vulkan_on_dxgi_desc') }}</div>
            </div>

            <!-- NVENC H264 CAVLC -->
            <div>
              <label for="nvenc_h264_cavlc" class="form-label">{{ $t('config.nvenc_h264_cavlc') }}</label>
              <select id="nvenc_h264_cavlc" class="form-select" v-model="config.nvenc_h264_cavlc">
                <option value="disabled">{{ $t('_common.disabled_def') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.nvenc_h264_cavlc_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>

</style>
