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
      <div class="card shadow-sm mb-4">
        <div class="card-header bg-dark bg-opacity-10 border-bottom-0">
          <div class="d-flex justify-content-between align-items-center flex-wrap gap-2">
            <h5 class="card-title mb-0">
              <i class="fas fa-file-alt text-dark me-2"></i>
              {{ $t('troubleshooting.logs') }}
            </h5>
            <div class="d-flex align-items-center gap-2">
              <div class="input-group" style="width: 280px">
                <span class="input-group-text bg-white">
                  <i class="fas fa-search text-muted"></i>
                </span>
                <input
                  type="text"
                  class="form-control"
                  v-model="logFilter"
                  :placeholder="$t('troubleshooting.logs_find')"
                />
              </div>
              <button class="btn btn-outline-primary" @click="handleCopyConfig">
                <i class="fas fa-copy me-1"></i>
                {{ $t('troubleshooting.copy_config') }}
              </button>
            </div>
          </div>
        </div>
        <div class="card-body">
          <p class="text-muted mb-3">{{ $t('troubleshooting.logs_desc') }}</p>
          <div class="logs-container">
            <button class="copy-btn" @click="copyLogs" title="Copy logs">
              <i class="fas fa-copy"></i>
            </button>
            <pre class="logs-content">{{ actualLogs }}</pre>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import Navbar from '../components/layout/Navbar.vue'
import TroubleshootingCard from '../components/TroubleshootingCard.vue'
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
.page-title {
  font-size: 1.75rem;
  font-weight: 600;
  color: #2c3e50;
  margin-bottom: 0.5rem;
}

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

.logs-container {
  position: relative;
  background: #1e1e1e;
  border-radius: 10px;
  overflow: hidden;
}

.logs-content {
  margin: 0;
  padding: 1.25rem;
  font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
  font-size: 0.85rem;
  line-height: 1.5;
  color: #d4d4d4;
  overflow: auto;
  max-height: 450px;
  min-height: 300px;
  white-space: pre-wrap;
  word-break: break-all;
}

.copy-btn {
  position: absolute;
  top: 12px;
  right: 12px;
  padding: 8px 12px;
  cursor: pointer;
  color: #ffffff;
  background: rgba(255, 255, 255, 0.1);
  border: none;
  border-radius: 6px;
  transition: all 0.2s ease;
  z-index: 10;
}

.copy-btn:hover {
  background: rgba(255, 255, 255, 0.2);
  transform: scale(1.05);
}

.copy-btn:active {
  transform: scale(0.95);
}

.input-group-text {
  border-right: none;
}

.input-group .form-control {
  border-left: none;
}

.input-group .form-control:focus {
  border-color: #ced4da;
  box-shadow: none;
}

.input-group:focus-within {
  box-shadow: 0 0 0 0.25rem rgba(13, 110, 253, 0.25);
  border-radius: 0.375rem;
}

.input-group:focus-within .input-group-text,
.input-group:focus-within .form-control {
  border-color: #86b7fe;
}

@media (max-width: 991.98px) {
  .page-title {
    font-size: 1.5rem;
  }

  .card-header .d-flex {
    flex-direction: column;
    align-items: flex-start !important;
  }

  .card-header .input-group {
    width: 100% !important;
    margin-top: 0.5rem;
  }
}
</style>
