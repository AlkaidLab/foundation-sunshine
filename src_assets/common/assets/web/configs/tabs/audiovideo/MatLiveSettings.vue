<script setup>
import { computed, onBeforeUnmount, onMounted } from 'vue'
import { useMatLiveStatus } from '../../../composables/useMatLiveStatus.js'
import { validateMatLiveSettings } from '../../../utils/matLiveSettings.js'

const props = defineProps(['config'])
const pairError = computed(() => validateMatLiveSettings(props.config))
const { status, loading, refresh, dispose } = useMatLiveStatus()
const knownStates = new Set(['disabled', 'incomplete', 'audio_disabled', 'unobserved', 'ready', 'streaming', 'idle', 'error', 'unsupported'])
const state = computed(() => knownStates.has(status.value?.state) ? status.value.state : 'unavailable')
onMounted(refresh)
onBeforeUnmount(dispose)
</script>

<template>
  <details class="mb-3 border rounded p-3">
    <summary class="fw-semibold">{{ $t('config.mat_live_title') }}</summary>
    <p class="form-text mt-2">{{ $t('config.mat_live_description') }}</p>
    <div class="mb-3">
      <label for="mat_live_pipe" class="form-label">{{ $t('config.mat_live_pipe') }}</label>
      <input id="mat_live_pipe" v-model="config.mat_live_pipe" type="text" class="form-control"
        :class="{ 'is-invalid': pairError }" :aria-invalid="!!pairError" aria-describedby="mat-live-validation" />
    </div>
    <div class="mb-3">
      <label for="mat_live_producer" class="form-label">{{ $t('config.mat_live_producer') }}</label>
      <input id="mat_live_producer" v-model="config.mat_live_producer" type="text" class="form-control"
        :class="{ 'is-invalid': pairError }" :aria-invalid="!!pairError" aria-describedby="mat-live-validation" />
    </div>
    <p id="mat-live-validation" :class="pairError ? 'text-danger small' : 'form-text'">
      {{ $t(pairError || 'config.mat_live_pair_note') }}
    </p>
    <div class="d-flex gap-2 align-items-center">
      <span role="status" aria-live="polite">
        {{ $t('config.mat_live_status') }}: {{ $t(loading ? 'config.mat_live_loading' : `config.mat_live_state_${state}`) }}
      </span>
      <button type="button" class="btn btn-sm btn-outline-secondary" :disabled="loading" @click="refresh">
        {{ $t('config.mat_live_refresh') }}
      </button>
    </div>
    <details v-if="status?.last_error" class="small mt-2">
      <summary>{{ $t('config.mat_live_error_details') }}</summary>
      <p class="text-break mb-0">{{ status.last_error }}</p>
    </details>
    <p class="form-text mb-0">{{ $t('config.mat_live_status_note') }}</p>
  </details>
</template>
