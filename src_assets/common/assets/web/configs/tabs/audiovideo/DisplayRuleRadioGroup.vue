<script setup>
import { $tp } from '../../../platform-i18n'

defineProps({
  modelValue: String,
  name: {
    type: String,
    required: true,
  },
  labelKey: {
    type: String,
    required: true,
  },
  optionKeyPrefix: {
    type: String,
    required: true,
  },
  options: {
    type: Array,
    required: true,
  },
  optionLabels: {
    type: Object,
    default: () => ({}),
  },
  platformAware: {
    type: Boolean,
    default: true,
  },
  disabled: {
    type: Boolean,
    default: false,
  },
})

const emit = defineEmits(['update:modelValue'])
</script>

<template>
  <section class="display-setting-card" :class="{ 'is-disabled': disabled }">
    <h3 :id="`${name}_label`" class="display-setting-title">
      {{ platformAware ? $tp(labelKey) : $t(labelKey) }}
    </h3>
    <div class="display-rule-options" role="radiogroup" :aria-labelledby="`${name}_label`">
      <label
        v-for="option in options"
        :key="option"
        class="display-rule-option"
        :class="{ 'is-selected': modelValue === option }"
      >
        <input
          class="form-check-input"
          type="radio"
          :name="name"
          :value="option"
          :checked="modelValue === option"
          :disabled="disabled"
          @change="emit('update:modelValue', option)"
        />
        <span>{{ optionLabels[option] || (platformAware ? $tp(optionKeyPrefix + option) : $t(optionKeyPrefix + option)) }}</span>
      </label>
    </div>
    <slot></slot>
  </section>
</template>

<style scoped>
.display-setting-card {
  min-width: 0;
  padding: 0.85rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  transition:
    opacity 0.25s ease,
    border-color 0.25s ease,
    box-shadow 0.25s ease;
}

.display-setting-card.is-disabled {
  opacity: 0.62;
  border-color: var(--ui-accent);
  box-shadow: 0 0 0 1px color-mix(in srgb, var(--ui-accent) 20%, transparent);
}

.display-setting-card.is-disabled .display-setting-title::after {
  content: ' 🔒';
  font-size: 0.72rem;
}

.display-setting-title {
  margin: 0 0 0.6rem;
  color: var(--ui-text-primary);
  font-size: 0.88rem;
  font-weight: 650;
}

.display-rule-options {
  display: grid;
  gap: 0.4rem;
}

.display-rule-option {
  display: flex;
  align-items: flex-start;
  gap: 0.5rem;
  margin: 0;
  padding: 0.55rem 0.65rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  color: var(--ui-text-secondary);
  font-size: 0.8rem;
  line-height: 1.35;
  cursor: pointer;
  transition:
    border-color 0.2s ease,
    background-color 0.2s ease,
    color 0.2s ease,
    opacity 0.25s ease;
}

.display-rule-option:hover {
  border-color: var(--ui-border-strong);
  background: var(--ui-surface-hover);
}

.display-rule-option.is-selected {
  border-color: var(--ui-accent);
  background: var(--ui-accent-soft);
  color: var(--ui-text-primary);
}

.display-rule-option:has(input:disabled) {
  opacity: 0.45;
  cursor: not-allowed;
  pointer-events: none;
}

.display-rule-option .form-check-input {
  flex: 0 0 auto;
  margin: 0.1rem 0 0;
  cursor: pointer;
}

.display-rule-option .form-check-input:disabled {
  cursor: not-allowed;
}
</style>
