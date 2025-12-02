<template>
  <div>
    <Navbar />
    <div class="container py-4">
      <div class="page-header mb-4">
        <h1 class="page-title">
          <i class="fas fa-tools me-3"></i>
          {{ $t('troubleshooting.troubleshooting') }}
        </h1>
      </div>

      <div class="row">
        <!-- Left Column -->
        <div class="col-lg-6">
          <!-- Force Close App -->
          <TroubleshootingCard
            icon="fa-times-circle"
            color="warning"
            :title="$t('troubleshooting.force_close')"
            :description="$t('troubleshooting.force_close_desc')"
          >
            <template #alerts>
              <div class="alert alert-success d-flex align-items-center" v-if="closeAppStatus === true">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.force_close_success') }}
              </div>
              <div class="alert alert-danger d-flex align-items-center" v-if="closeAppStatus === false">
                <i class="fas fa-exclamation-circle me-2"></i>
                {{ $t('troubleshooting.force_close_error') }}
              </div>
            </template>
            <button class="btn btn-warning" :disabled="closeAppPressed" @click="closeApp">
              <i class="fas fa-times me-2"></i>
              {{ $t('troubleshooting.force_close') }}
            </button>
          </TroubleshootingCard>

          <!-- Restart Sunshine -->
          <TroubleshootingCard
            icon="fa-sync-alt"
            color="info"
            :title="$t('troubleshooting.restart_sunshine')"
            :description="$t('troubleshooting.restart_sunshine_desc')"
          >
            <template #alerts>
              <div class="alert alert-success d-flex align-items-center" v-if="restartPressed">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.restart_sunshine_success') }}
              </div>
            </template>
            <button class="btn btn-info text-white" :disabled="restartPressed" @click="restart">
              <i class="fas fa-redo me-2"></i>
              {{ $t('troubleshooting.restart_sunshine') }}
            </button>
          </TroubleshootingCard>

          <!-- Boom Sunshine -->
          <TroubleshootingCard
            icon="fa-bomb"
            color="danger"
            :title="$t('troubleshooting.boom_sunshine')"
            :description="$t('troubleshooting.boom_sunshine_desc')"
          >
            <template #alerts>
              <div class="alert alert-success d-flex align-items-center" v-if="boomPressed">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.boom_sunshine_success') }}
              </div>
            </template>
            <button class="btn btn-danger" :disabled="boomPressed" @click="handleConfirmBoom">
              <i class="fas fa-bomb me-2"></i>
              {{ $t('troubleshooting.boom_sunshine') }}
            </button>
          </TroubleshootingCard>
        </div>

        <!-- Right Column -->
        <div class="col-lg-6">
          <!-- Reset persistent display device settings -->
          <TroubleshootingCard
            v-if="platform === 'windows'"
            icon="fa-desktop"
            color="secondary"
            :title="$t('troubleshooting.reset_display_device_windows')"
            :description="$t('troubleshooting.reset_display_device_desc_windows')"
            pre-line
          >
            <template #alerts>
              <div class="alert alert-success d-flex align-items-center" v-if="resetDisplayDeviceStatus === true">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.reset_display_device_success_windows') }}
              </div>
              <div class="alert alert-danger d-flex align-items-center" v-if="resetDisplayDeviceStatus === false">
                <i class="fas fa-exclamation-circle me-2"></i>
                {{ $t('troubleshooting.reset_display_device_error_windows') }}
              </div>
            </template>
            <button
              class="btn btn-secondary"
              :disabled="resetDisplayDevicePressed"
              @click="resetDisplayDevicePersistence"
            >
              <i class="fas fa-undo me-2"></i>
              {{ $t('troubleshooting.reset_display_device_windows') }}
            </button>
          </TroubleshootingCard>

          <!-- Reopen Setup Wizard -->
          <TroubleshootingCard
            icon="fa-magic"
            color="primary"
            :title="$t('troubleshooting.reopen_setup_wizard')"
            :description="$t('troubleshooting.reopen_setup_wizard_desc')"
          >
            <button class="btn btn-primary" @click="handleReopenSetupWizard">
              <i class="fas fa-redo me-2"></i>
              {{ $t('troubleshooting.reopen_setup_wizard') }}
            </button>
          </TroubleshootingCard>
        </div>
      </div>

      <!-- Logs Section - Full Width -->
      <LogsSection
        v-model:logFilter="logFilter"
        v-model:matchMode="matchMode"
        v-model:ignoreCase="ignoreCase"
        :actualLogs="actualLogs"
        :copyLogs="copyLogs"
        :copyConfig="handleCopyConfig"
      />
    </div>
  </div>
</template>

<script setup>
import { onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import Navbar from '../components/layout/Navbar.vue'
import TroubleshootingCard from '../components/TroubleshootingCard.vue'
import LogsSection from '../components/LogsSection.vue'
import { useTroubleshooting } from '../composables/useTroubleshooting.js'

const { t } = useI18n()

const {
  platform,
  closeAppPressed,
  closeAppStatus,
  restartPressed,
  boomPressed,
  resetDisplayDevicePressed,
  resetDisplayDeviceStatus,
  logFilter,
  matchMode,
  ignoreCase,
  actualLogs,
  refreshLogs,
  closeApp,
  restart,
  boom,
  resetDisplayDevicePersistence,
  copyLogs,
  copyConfig,
  reopenSetupWizard,
  loadPlatform,
  startLogRefresh,
} = useTroubleshooting()

const handleConfirmBoom = async () => {
  if (await confirm(t('troubleshooting.confirm_boom_desc'))) {
    boom()
  }
}

const handleCopyConfig = () => copyConfig(t)

const handleReopenSetupWizard = () => reopenSetupWizard(t)

onMounted(async () => {
  await Promise.all([loadPlatform(), refreshLogs()])
  startLogRefresh()
})
</script>

<style>
@import '../styles/global.css';
</style>

<style scoped>
.card {
  border: none;
  border-radius: 12px;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}

.card:hover {
  transform: translateY(-2px);
  box-shadow: 0 8px 25px rgba(0, 0, 0, 0.1) !important;
}

.card-header {
  border-radius: 12px 12px 0 0 !important;
  padding: 1rem 1.25rem;
}

.card-title {
  font-size: 1rem;
  font-weight: 600;
}

.card-body {
  padding: 1.25rem;
}

.card-body p {
  font-size: 0.9rem;
  line-height: 1.6;
}

.btn {
  border-radius: 8px;
  padding: 0.5rem 1rem;
  font-weight: 500;
  transition: all 0.2s ease;
}

.btn:hover {
  transform: translateY(-1px);
}

.alert {
  border-radius: 8px;
  font-size: 0.9rem;
  padding: 0.75rem 1rem;
}

@media (max-width: 991.98px) {
  .page-title {
    font-size: 1.5rem;
  }
}
</style>
