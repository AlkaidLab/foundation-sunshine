<script setup>
import { onMounted } from 'vue'
import { useVddStatus } from '../../composables/useVddStatus.js'

defineProps({
  active: { type: Boolean, default: true },
})

const {
  vddStatus,
  vddStatusLoading,
  vddInstalling,
  vddStatusError,
  canManageVdd,
  vddReady,
  refreshVddStatus,
  installVdd,
} = useVddStatus()

async function installOrRepair() {
  try {
    await installVdd()
  } catch {
    // The composable exposes the actionable error inline.
  }
}

onMounted(refreshVddStatus)
</script>

<template>
  <div v-if="active && !vddReady" class="alert alert-warning mt-2 mb-3 vdd-prerequisite-notice">
    <div class="d-flex align-items-start gap-2">
      <i class="fas fa-exclamation-triangle mt-1"></i>
      <div class="flex-grow-1">
        <strong>{{ $t('config.vdd_driver_required') }}</strong>
        <div class="small mt-1">
          {{ canManageVdd ? $t('config.vdd_driver_desktop_hint') : $t('config.vdd_driver_browser_hint') }}
        </div>
        <div role="status" aria-live="polite" aria-atomic="true">
          <div v-if="vddStatusError" class="small mt-1">{{ vddStatusError }}</div>
          <div v-else-if="vddStatus.state !== 'unknown'" class="small mt-1">
            {{ $t(`config.vdd_driver_state_${vddStatus.state}`) }}
          </div>
        </div>
        <div class="d-flex gap-2 mt-2">
          <button
            v-if="canManageVdd"
            type="button"
            class="btn btn-sm btn-warning"
            :disabled="vddInstalling || vddStatusLoading"
            @click="installOrRepair"
          >
            {{ vddInstalling ? $t('config.vdd_driver_installing') : $t('config.vdd_driver_install_repair') }}
          </button>
          <button
            type="button"
            class="btn btn-sm btn-outline-secondary"
            :disabled="vddInstalling || vddStatusLoading"
            @click="refreshVddStatus"
          >
            {{ $t('config.vdd_driver_recheck') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
