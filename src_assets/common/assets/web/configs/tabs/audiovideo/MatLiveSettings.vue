<script setup>
import { computed, onBeforeUnmount, onMounted } from 'vue'
import Checkbox from '../../../components/Checkbox.vue'
import { useMatLiveStatus } from '../../../composables/useMatLiveStatus.js'

defineProps(['config'])
const { status, loading, start, dispose } = useMatLiveStatus()
const knownStates = new Set(['disabled', 'audio_disabled', 'unobserved', 'ready', 'streaming', 'idle', 'error', 'unsupported'])
const state = computed(() => knownStates.has(status.value?.state) ? status.value.state : 'unavailable')
onMounted(start)
onBeforeUnmount(dispose)
</script>

<template>
  <div class="mb-3 border rounded p-3">
    <Checkbox id="mat_live" v-model="config.mat_live" true-value="enabled" false-value="disabled" />
    <div class="small mt-2" role="status" aria-live="polite">
      {{ $t('config.mat_live_status') }}: {{ $t(loading && !status ? 'config.mat_live_loading' : `config.mat_live_state_${state}`) }}
    </div>
  </div>
</template>
