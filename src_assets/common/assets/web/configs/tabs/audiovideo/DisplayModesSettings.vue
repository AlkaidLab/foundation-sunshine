<script setup>
import { ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div class="mb-3">

    <!--max_bitrate-->
    <div class="mb-3">
      <label for="max_bitrate" class="form-label">{{ $t("config.max_bitrate") }}</label>
      <input type="number" class="form-control" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
      <div class="form-text">{{ $t("config.max_bitrate_desc") }}</div>
    </div>

    <!-- Variable Refresh Rate -->
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="variable_refresh_rate" v-model="config.variable_refresh_rate" true-value="enabled" false-value="disabled" />
        <label class="form-check-label" for="variable_refresh_rate">
          {{ $t("config.variable_refresh_rate") }}
        </label>
      </div>
      <div class="form-text">{{ $t("config.variable_refresh_rate_desc") }}</div>
    </div>

    <!-- Minimum FPS Target -->
    <div class="mb-3">
      <label for="minimum_fps_target" class="form-label">{{ $t("config.minimum_fps_target") }}</label>
      <input type="number" class="form-control" id="minimum_fps_target" placeholder="0" v-model.number="config.minimum_fps_target" min="0" max="1000" />
      <div class="form-text">{{ $t("config.minimum_fps_target_desc") }}</div>
    </div>

  </div>
</template>

<style scoped>
</style>
