<script setup>
import { ref } from 'vue'

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)

function useAutomaticValue(field) {
  config.value[field] = 0
}
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
      <div class="stream-limit-field">
        <div class="stream-limit-heading">
          <label for="max_bitrate" class="setting-title">
            <i class="fas fa-gauge-high" aria-hidden="true"></i>
            {{ $t('config.max_bitrate') }}
          </label>
          <span v-if="Number(config.max_bitrate) === 0" class="automatic-badge">
            {{ $t('_common.auto') }}
          </span>
        </div>
        <div class="input-group">
          <input
            id="max_bitrate"
            v-model="config.max_bitrate"
            type="number"
            min="0"
            class="form-control"
            placeholder="0"
            aria-describedby="max_bitrate_desc"
          />
          <span class="input-group-text">Kbps</span>
          <button
            type="button"
            class="btn btn-outline-secondary automatic-button"
            :title="$t('_common.auto')"
            @click="useAutomaticValue('max_bitrate')"
          >
            <i class="fas fa-rotate-left" aria-hidden="true"></i>
            <span>{{ $t('_common.auto') }}</span>
          </button>
        </div>
        <div id="max_bitrate_desc" class="form-text">
          {{ $t('config.max_bitrate_desc') }}
        </div>
      </div>

      <div class="stream-limit-field">
        <div class="stream-limit-heading">
          <label for="minimum_fps_target" class="setting-title">
            <i class="fas fa-stopwatch" aria-hidden="true"></i>
            {{ $t('config.minimum_fps_target') }}
          </label>
          <span v-if="Number(config.minimum_fps_target) === 0" class="automatic-badge">
            {{ $t('_common.auto') }}
          </span>
        </div>
        <div class="input-group">
          <input
            id="minimum_fps_target"
            v-model.number="config.minimum_fps_target"
            type="number"
            min="0"
            max="1000"
            class="form-control"
            placeholder="0"
            aria-describedby="minimum_fps_target_desc"
          />
          <span class="input-group-text">FPS</span>
          <button
            type="button"
            class="btn btn-outline-secondary automatic-button"
            :title="$t('_common.auto')"
            @click="useAutomaticValue('minimum_fps_target')"
          >
            <i class="fas fa-rotate-left" aria-hidden="true"></i>
            <span>{{ $t('_common.auto') }}</span>
          </button>
        </div>
        <div id="minimum_fps_target_desc" class="form-text">
          {{ $t('config.minimum_fps_target_desc') }}
        </div>
      </div>
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

.stream-limit-heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  margin-bottom: 0.65rem;
}

.automatic-badge {
  flex: 0 0 auto;
  padding: 0.15rem 0.5rem;
  border-radius: 999px;
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
  font-size: 0.72rem;
  font-weight: 650;
}

.input-group {
  flex-wrap: nowrap;
}

.input-group-text {
  color: var(--ui-text-secondary);
  font-size: 0.78rem;
}

.automatic-button {
  display: inline-flex;
  align-items: center;
  gap: 0.35rem;
  white-space: nowrap;
}

.stream-limit-field .form-text {
  margin-top: 0.55rem;
  line-height: 1.45;
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

  .automatic-button span {
    display: none;
  }
}
</style>
