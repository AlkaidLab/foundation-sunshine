<script setup>
import { $tp } from '../../../platform-i18n'

defineProps({
  modelValue: String,
})

const emit = defineEmits(['update:modelValue'])

const modes = [
  'no_operation',
  'ensure_active',
  'ensure_primary',
  'ensure_secondary',
  'ensure_only_display',
]
</script>

<template>
  <fieldset class="display-prep-group">
    <legend class="form-label fw-semibold mb-2">
      {{ $tp('config.display_device_prep') }}
    </legend>
    <div class="display-prep-options">
      <label
        v-for="mode in modes"
        :key="mode"
        class="display-prep-option"
        :class="{ 'is-selected': modelValue === mode }"
      >
        <input
          class="visually-hidden"
          type="radio"
          name="display_device_prep"
          :value="mode"
          :checked="modelValue === mode"
          @change="emit('update:modelValue', mode)"
        />
        <div class="display-prep-option-content">
          <div class="topology-preview" :class="`is-${mode}`" aria-hidden="true">
            <div class="topology-screen topology-host">
              <i class="fas fa-desktop"></i>
              <span class="topology-off-mark"></span>
            </div>
            <span class="topology-link"></span>
            <div class="topology-screen topology-target">
              <i class="fas fa-desktop"></i>
              <span class="topology-primary-badge">
                <i class="fas fa-star"></i>
              </span>
            </div>
          </div>
          <div class="display-prep-copy">
            <span class="display-prep-title">
              {{ $tp('config.display_device_prep_' + mode) }}
            </span>
            <span class="display-prep-description">
              {{ $tp('config.display_device_prep_' + mode + '_desc') }}
            </span>
          </div>
        </div>
      </label>
    </div>
  </fieldset>
</template>

<style scoped>
.display-prep-group {
  min-width: 0;
  margin: 0 0 1rem;
  padding: 0;
  border: 0;
}

.display-prep-options {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 17rem), 1fr));
  gap: 0.75rem;
}

.display-prep-option {
  min-width: 0;
  margin: 0;
  cursor: pointer;
}

.display-prep-option-content {
  display: grid;
  grid-template-columns: 7rem minmax(0, 1fr);
  align-items: center;
  gap: 0.85rem;
  height: 100%;
  padding: 0.85rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  transition:
    border-color 0.2s ease,
    background-color 0.2s ease,
    box-shadow 0.2s ease,
    transform 0.2s ease;
}

.display-prep-option:hover .display-prep-option-content {
  border-color: var(--ui-border-strong);
  background: var(--ui-surface-hover);
  transform: translateY(-1px);
}

.display-prep-option.is-selected .display-prep-option-content {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  box-shadow: 0 0 0 1px color-mix(in srgb, var(--ui-accent) 28%, transparent);
}

.display-prep-option input:focus-visible + .display-prep-option-content {
  outline: 2px solid var(--ui-accent);
  outline-offset: 2px;
}

.display-prep-copy {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 0.25rem;
}

.display-prep-title {
  color: var(--ui-text-primary);
  font-size: 0.9rem;
  font-weight: 650;
}

.display-prep-description {
  color: var(--ui-text-secondary);
  font-size: 0.78rem;
  line-height: 1.4;
}

.topology-preview {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 3.5rem;
}

.topology-screen {
  position: relative;
  display: grid;
  width: 2.65rem;
  height: 1.75rem;
  place-items: center;
  border: 2px solid var(--ui-border-strong);
  border-radius: 0.3rem;
  background: var(--ui-surface-strong);
  color: var(--ui-text-muted);
}

.topology-screen::after {
  position: absolute;
  bottom: -0.38rem;
  width: 1rem;
  height: 0.25rem;
  border-top: 2px solid currentColor;
  content: '';
}

.topology-screen i {
  font-size: 0.8rem;
}

.topology-link {
  width: 0.75rem;
  border-top: 2px solid var(--ui-border-strong);
}

.topology-primary-badge {
  position: absolute;
  top: -0.45rem;
  right: -0.45rem;
  display: none;
  width: 1rem;
  height: 1rem;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  background: var(--ui-accent);
  color: var(--ui-on-accent, #fff);
  font-size: 0.48rem;
  box-shadow: 0 0 0 2px var(--ui-surface);
}

.topology-off-mark {
  position: absolute;
  display: none;
  width: 2.8rem;
  border-top: 2px solid var(--ui-warning-text);
  transform: rotate(-34deg);
}

.is-no_operation .topology-link,
.is-ensure_only_display .topology-link {
  border-top-style: dashed;
}

.is-ensure_active .topology-target,
.is-ensure_primary .topology-target,
.is-ensure_only_display .topology-target {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
}

.is-ensure_primary .topology-primary-badge {
  display: flex;
}

.is-ensure_secondary .topology-screen {
  border-color: var(--ui-accent);
  color: var(--ui-accent);
}

.is-ensure_secondary .topology-target {
  background: var(--ui-accent-soft);
}

.is-ensure_only_display .topology-host {
  opacity: 0.38;
}

.is-ensure_only_display .topology-off-mark {
  display: block;
}

.is-ensure_only_display .topology-link {
  opacity: 0.45;
}

@media (max-width: 575.98px) {
  .display-prep-option-content {
    grid-template-columns: 6.5rem minmax(0, 1fr);
  }
}
</style>
