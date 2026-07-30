<script setup>
const props = defineProps({
  modelValue: [Number, String],
  id: {
    type: String,
    required: true,
  },
  labelKey: {
    type: String,
    required: true,
  },
  descriptionKey: {
    type: String,
    required: true,
  },
  icon: {
    type: String,
    required: true,
  },
  unit: {
    type: String,
    required: true,
  },
  min: {
    type: Number,
    default: 0,
  },
  max: {
    type: Number,
    default: undefined,
  },
  coerceNumber: Boolean,
})

const emit = defineEmits(['update:modelValue'])

function updateValue(event) {
  const value = event.target.value
  emit('update:modelValue', props.coerceNumber && value !== '' ? Number(value) : value)
}

function useAutomaticValue() {
  emit('update:modelValue', 0)
}
</script>

<template>
  <div class="automatic-number-setting">
    <div class="setting-heading">
      <label :for="id" class="setting-title">
        <i class="fas" :class="icon" aria-hidden="true"></i>
        {{ $t(labelKey) }}
      </label>
      <span v-if="Number(modelValue) === 0" class="automatic-badge">
        {{ $t('_common.auto') }}
      </span>
    </div>
    <div class="input-group">
      <input
        :id="id"
        :value="modelValue"
        type="number"
        :min="min"
        :max="max"
        class="form-control"
        placeholder="0"
        :aria-describedby="`${id}_desc`"
        @input="updateValue"
      />
      <span class="input-group-text">{{ unit }}</span>
      <button
        type="button"
        class="btn btn-outline-secondary automatic-button"
        @click="useAutomaticValue"
      >
        <i class="fas fa-rotate-left" aria-hidden="true"></i>
        <span>{{ $t('_common.auto') }}</span>
      </button>
    </div>
    <div :id="`${id}_desc`" class="form-text">
      {{ $t(descriptionKey) }}
    </div>
  </div>
</template>

<style scoped>
.setting-heading {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 0.75rem;
  margin-bottom: 0.65rem;
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

.form-text {
  margin-top: 0.55rem;
  line-height: 1.45;
}

@media (max-width: 575.98px) {
  .automatic-button span {
    display: none;
  }
}
</style>
