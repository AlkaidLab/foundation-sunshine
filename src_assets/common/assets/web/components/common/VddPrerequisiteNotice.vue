<script setup>
import { onMounted } from 'vue'
import { MBanner, MButton } from 'foundation-ui/vue'
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
  <MBanner v-if="active && !vddReady" class="vdd-prerequisite-notice" tone="warn" icon="warning">
    <strong>{{ $t('config.vdd_driver_required') }}</strong>
    <div>{{ canManageVdd ? $t('config.vdd_driver_desktop_hint') : $t('config.vdd_driver_browser_hint') }}</div>
    <div role="status" aria-live="polite" aria-atomic="true">
      <template v-if="vddStatusError">{{ vddStatusError }}</template>
      <template v-else-if="vddStatus.state !== 'unknown'">{{ $t($te(`config.vdd_driver_state_${vddStatus.state}`) ? `config.vdd_driver_state_${vddStatus.state}` : 'config.vdd_driver_state_unknown') }}</template>
    </div>
    <template #actions>
      <MButton v-if="canManageVdd" variant="text" size="sm" :loading="vddInstalling" :disabled="vddStatusLoading" @click="installOrRepair">
        {{ vddInstalling ? $t('config.vdd_driver_installing') : $t('config.vdd_driver_install_repair') }}
      </MButton>
      <MButton variant="text" size="sm" :disabled="vddInstalling || vddStatusLoading" @click="refreshVddStatus">{{ $t('config.vdd_driver_recheck') }}</MButton>
    </template>
  </MBanner>
</template>
