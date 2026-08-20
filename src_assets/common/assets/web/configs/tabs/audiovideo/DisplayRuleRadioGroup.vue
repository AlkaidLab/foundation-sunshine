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
})

const emit = defineEmits(['update:modelValue'])
</script>

<template>
  <section class="display-setting-card">
    <h3 :id="`${name}_label`" class="display-setting-title">
      {{ $tp(labelKey) }}
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
          @change="emit('update:modelValue', option)"
        />
        <span>{{ $tp(optionKeyPrefix + option) }}</span>
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
    color 0.2s ease;
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

.display-rule-option .form-check-input {
  flex: 0 0 auto;
  margin: 0.1rem 0 0;
  cursor: pointer;
}
</style>
