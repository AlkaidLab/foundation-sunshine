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
          <div class="card shadow-sm mb-4">
            <div class="card-header bg-warning bg-opacity-10 border-bottom-0">
              <h5 class="card-title mb-0">
                <i class="fas fa-times-circle text-warning me-2"></i>
                {{ $t('troubleshooting.force_close') }}
              </h5>
            </div>
            <div class="card-body">
              <p class="text-muted mb-3">{{ $t('troubleshooting.force_close_desc') }}</p>
              <div class="alert alert-success d-flex align-items-center" v-if="closeAppStatus === true">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.force_close_success') }}
              </div>
              <div class="alert alert-danger d-flex align-items-center" v-if="closeAppStatus === false">
                <i class="fas fa-exclamation-circle me-2"></i>
                {{ $t('troubleshooting.force_close_error') }}
              </div>
              <button class="btn btn-warning" :disabled="closeAppPressed" @click="closeApp">
                <i class="fas fa-times me-2"></i>
                {{ $t('troubleshooting.force_close') }}
              </button>
            </div>
          </div>

          <!-- Restart Sunshine -->
          <div class="card shadow-sm mb-4">
            <div class="card-header bg-info bg-opacity-10 border-bottom-0">
              <h5 class="card-title mb-0">
                <i class="fas fa-sync-alt text-info me-2"></i>
                {{ $t('troubleshooting.restart_sunshine') }}
              </h5>
            </div>
            <div class="card-body">
              <p class="text-muted mb-3">{{ $t('troubleshooting.restart_sunshine_desc') }}</p>
              <div class="alert alert-success d-flex align-items-center" v-if="restartPressed === true">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.restart_sunshine_success') }}
              </div>
              <button class="btn btn-info text-white" :disabled="restartPressed" @click="restart">
                <i class="fas fa-redo me-2"></i>
                {{ $t('troubleshooting.restart_sunshine') }}
              </button>
            </div>
          </div>

          <!-- Boom Sunshine -->
          <div class="card shadow-sm mb-4">
            <div class="card-header bg-danger bg-opacity-10 border-bottom-0">
              <h5 class="card-title mb-0">
                <i class="fas fa-bomb text-danger me-2"></i>
                {{ $t('troubleshooting.boom_sunshine') }}
              </h5>
            </div>
            <div class="card-body">
              <p class="text-muted mb-3">{{ $t('troubleshooting.boom_sunshine_desc') }}</p>
              <div class="alert alert-success d-flex align-items-center" v-if="boomPressed === true">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.boom_sunshine_success') }}
              </div>
              <button class="btn btn-danger" :disabled="boomPressed" @click="confirmBoom">
                <i class="fas fa-bomb me-2"></i>
                {{ $t('troubleshooting.boom_sunshine') }}
              </button>
            </div>
          </div>
        </div>

        <!-- Right Column -->
        <div class="col-lg-6">
          <!-- Reset persistent display device settings -->
          <div class="card shadow-sm mb-4" v-if="platform === 'windows'">
            <div class="card-header bg-secondary bg-opacity-10 border-bottom-0">
              <h5 class="card-title mb-0">
                <i class="fas fa-desktop text-secondary me-2"></i>
                {{ $t('troubleshooting.reset_display_device_windows') }}
              </h5>
            </div>
            <div class="card-body">
              <p class="text-muted mb-3" style="white-space: pre-line">
                {{ $t('troubleshooting.reset_display_device_desc_windows') }}
              </p>
              <div class="alert alert-success d-flex align-items-center" v-if="resetDisplayDeviceStatus === true">
                <i class="fas fa-check-circle me-2"></i>
                {{ $t('troubleshooting.reset_display_device_success_windows') }}
              </div>
              <div class="alert alert-danger d-flex align-items-center" v-if="resetDisplayDeviceStatus === false">
                <i class="fas fa-exclamation-circle me-2"></i>
                {{ $t('troubleshooting.reset_display_device_error_windows') }}
              </div>
              <button
                class="btn btn-secondary"
                :disabled="resetDisplayDevicePressed"
                @click="resetDisplayDevicePersistence"
              >
                <i class="fas fa-undo me-2"></i>
                {{ $t('troubleshooting.reset_display_device_windows') }}
              </button>
            </div>
          </div>

          <!-- Reopen Setup Wizard -->
          <div class="card shadow-sm mb-4">
            <div class="card-header bg-primary bg-opacity-10 border-bottom-0">
              <h5 class="card-title mb-0">
                <i class="fas fa-magic text-primary me-2"></i>
                {{ $t('troubleshooting.reopen_setup_wizard') }}
              </h5>
            </div>
            <div class="card-body">
              <p class="text-muted mb-3">{{ $t('troubleshooting.reopen_setup_wizard_desc') }}</p>
              <button class="btn btn-primary" @click="handleReopenSetupWizard">
                <i class="fas fa-redo me-2"></i>
                {{ $t('troubleshooting.reopen_setup_wizard') }}
              </button>
            </div>
          </div>
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

    <!-- Confirmation Boom Sunshine Modal -->
    <div id="boomModal" class="modal-overlay">
      <div class="modal-dialog">
        <div class="modal-header">
          <h5 class="modal-title">
            <i class="fas fa-exclamation-triangle text-danger me-2"></i>
            {{ $t('troubleshooting.confirm_boom') }}
          </h5>
          <button type="button" class="btn-close" @click="closeModal"></button>
        </div>
        <div class="modal-body">
          <p>{{ $t('troubleshooting.confirm_boom_desc') }}</p>
        </div>
        <div class="modal-footer">
          <button type="button" class="btn btn-secondary" @click="closeModal">
            <i class="fas fa-times me-1"></i>
            {{ $t('_common.cancel') }}
          </button>
          <button type="button" class="btn btn-danger" @click="boom">
            <i class="fas fa-bomb me-1"></i>
            {{ $t('troubleshooting.boom_sunshine') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import Navbar from '../components/layout/Navbar.vue'
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
  confirmBoom,
  closeModal,
  boom,
  resetDisplayDevicePersistence,
  copyLogs,
  copyConfig,
  reopenSetupWizard,
  loadPlatform,
  startLogRefresh,
} = useTroubleshooting()

const handleCopyConfig = () => {
  copyConfig(t)
}

const handleReopenSetupWizard = () => {
  reopenSetupWizard(t)
}

onMounted(async () => {
  await loadPlatform()
  await refreshLogs()
  startLogRefresh()
})
</script>

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

/* Modal styles */
.modal-overlay {
  position: fixed;
  z-index: 1050;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
  overflow: auto;
  background-color: rgba(0, 0, 0, 0.5);
  backdrop-filter: blur(4px);
  display: none;
  align-items: center;
  justify-content: center;
}

.modal-overlay.show {
  display: flex;
}

.modal-dialog {
  background-color: #ffffff;
  margin: auto;
  padding: 0;
  border: none;
  max-width: 450px;
  width: 90%;
  border-radius: 16px;
  box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
  animation: modalSlideIn 0.3s ease;
}

@keyframes modalSlideIn {
  from {
    opacity: 0;
    transform: translateY(-20px) scale(0.95);
  }
  to {
    opacity: 1;
    transform: translateY(0) scale(1);
  }
}

.modal-dialog .modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 1.25rem 1.5rem;
  border-bottom: 1px solid #e9ecef;
  border-radius: 16px 16px 0 0;
}

.modal-title {
  margin: 0;
  font-size: 1.1rem;
  font-weight: 600;
  color: #2c3e50;
}

.btn-close {
  background: transparent;
  border: none;
  font-size: 1.25rem;
  cursor: pointer;
  opacity: 0.5;
  transition: opacity 0.2s;
}

.btn-close:hover {
  opacity: 1;
}

.modal-dialog .modal-body {
  padding: 1.5rem;
  font-size: 0.95rem;
  line-height: 1.6;
  color: #495057;
}

.modal-dialog .modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 12px;
  padding: 1rem 1.5rem;
  border-top: 1px solid #e9ecef;
  border-radius: 0 0 16px 16px;
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
