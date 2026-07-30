<script setup>
import { ref } from 'vue'
import AutomaticNumberSetting from './AutomaticNumberSetting.vue'

const props = defineProps(['config'])

const config = ref(props.config)
</script>

<template>
  <section class="display-modes-card">
    <div class="vrr-setting">
      <div class="setting-copy">
        <label class="setting-title" for="variable_refresh_rate">
          <i class="fas fa-wave-square" aria-hidden="true"></i>
          {{ $t('config.variable_refresh_rate') }}
        </label>
        <div id="variable_refresh_rate_desc" class="form-text mt-1">
          {{ $t('config.variable_refresh_rate_desc') }}
        </div>
      </div>
      <div class="form-check form-switch mb-0">
        <input
          id="variable_refresh_rate"
          v-model="config.variable_refresh_rate"
          class="form-check-input"
          type="checkbox"
          role="switch"
          aria-describedby="variable_refresh_rate_desc"
          true-value="enabled"
          false-value="disabled"
        />
      </div>
    </div>

    <div class="stream-limit-grid">
      <AutomaticNumberSetting
        v-model="config.max_bitrate"
        class="stream-limit-field"
        id="max_bitrate"
        label-key="config.max_bitrate"
        description-key="config.max_bitrate_desc"
        icon="fa-gauge-high"
        unit="Kbps"
      />
      <AutomaticNumberSetting
        v-model="config.minimum_fps_target"
        class="stream-limit-field"
        id="minimum_fps_target"
        label-key="config.minimum_fps_target"
        description-key="config.minimum_fps_target_desc"
        icon="fa-stopwatch"
        unit="FPS"
        :max="1000"
        coerce-number
      />
    </div>
  </section>
</template>

<style scoped>
.display-modes-card {
  overflow: hidden;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

.vrr-setting {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 1rem;
  border-bottom: 1px solid var(--ui-border);
  background: var(--ui-accent-soft);
}

.setting-copy {
  min-width: 0;
}

.setting-title {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  margin: 0;
  color: var(--ui-text-primary);
  font-size: 0.9rem;
  font-weight: 650;
}

.setting-title i {
  width: 1rem;
  color: var(--ui-accent);
  text-align: center;
}

.vrr-setting .form-switch {
  flex: 0 0 auto;
  padding-left: 0;
}

.vrr-setting .form-check-input {
  float: none;
  margin: 0;
}

.stream-limit-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 18rem), 1fr));
}

.stream-limit-field {
  min-width: 0;
  padding: 1rem;
}

.stream-limit-field + .stream-limit-field {
  border-left: 1px solid var(--ui-border);
}

@media (max-width: 767.98px) {
  .stream-limit-field + .stream-limit-field {
    border-top: 1px solid var(--ui-border);
    border-left: 0;
  }
}

@media (max-width: 575.98px) {
  .vrr-setting,
  .stream-limit-field {
    padding: 0.85rem;
  }

}
</style>
