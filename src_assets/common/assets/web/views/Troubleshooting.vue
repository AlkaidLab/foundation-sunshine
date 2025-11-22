<template>
  <div>
    <Navbar />
    <div class="container">
      <h1 class="my-4">{{ $t('troubleshooting.troubleshooting') }}</h1>
      
      <!-- Force Close App -->
      <div class="card p-2 my-4">
        <div class="card-body">
          <h2 id="close_apps">{{ $t('troubleshooting.force_close') }}</h2>
          <br>
          <p>{{ $t('troubleshooting.force_close_desc') }}</p>
          <div class="alert alert-success" v-if="closeAppStatus === true">
            {{ $t('troubleshooting.force_close_success') }}
          </div>
          <div class="alert alert-danger" v-if="closeAppStatus === false">
            {{ $t('troubleshooting.force_close_error') }}
          </div>
          <div>
            <button class="btn btn-warning" :disabled="closeAppPressed" @click="closeApp">
              {{ $t('troubleshooting.force_close') }}
            </button>
          </div>
        </div>
      </div>
      
      <!-- Restart Sunshine -->
      <div class="card p-2 my-4">
        <div class="card-body">
          <h2 id="restart">{{ $t('troubleshooting.restart_sunshine') }}</h2>
          <br>
          <p>{{ $t('troubleshooting.restart_sunshine_desc') }}</p>
          <div class="alert alert-success" v-if="restartPressed === true">
            {{ $t('troubleshooting.restart_sunshine_success') }}
          </div>
          <div>
            <button class="btn btn-warning" :disabled="restartPressed" @click="restart">
              {{ $t('troubleshooting.restart_sunshine') }}
            </button>
          </div>
        </div>
      </div>
      
      <!-- Boom Sunshine -->
      <div class="card p-2 my-4">
        <div class="card-body">
          <h2 id="boom">{{ $t('troubleshooting.boom_sunshine') }}</h2>
          <br>
          <p>{{ $t('troubleshooting.boom_sunshine_desc') }}</p>
          <div class="alert alert-success" v-if="boomPressed === true">
            {{ $t('troubleshooting.boom_sunshine_success') }}
          </div>
          <div>
            <button class="btn btn-warning" :disabled="boomPressed" @click="confirmBoom">
              {{ $t('troubleshooting.boom_sunshine') }}
            </button>
          </div>
        </div>
      </div>
      
      <!-- Reset persistent display device settings -->
      <div class="card p-2 my-4" v-if="platform === 'windows'">
        <div class="card-body">
          <h2 id="reset_display_device">{{ $t('troubleshooting.reset_display_device_windows') }}</h2>
          <br>
          <p style="white-space: pre-line">{{ $t('troubleshooting.reset_display_device_desc_windows') }}</p>
          <div class="alert alert-success" v-if="resetDisplayDeviceStatus === true">
            {{ $t('troubleshooting.reset_display_device_success_windows') }}
          </div>
          <div class="alert alert-danger" v-if="resetDisplayDeviceStatus === false">
            {{ $t('troubleshooting.reset_display_device_error_windows') }}
          </div>
          <div>
            <button class="btn btn-warning" :disabled="resetDisplayDevicePressed" @click="resetDisplayDevicePersistence">
              {{ $t('troubleshooting.reset_display_device_windows') }}
            </button>
          </div>
        </div>
      </div>
      
      <!-- Reopen Setup Wizard -->
      <div class="card p-2 my-4">
        <div class="card-body">
          <h2 id="reopen_setup_wizard">{{ $t('troubleshooting.reopen_setup_wizard') }}</h2>
          <br>
          <p>{{ $t('troubleshooting.reopen_setup_wizard_desc') }}</p>
          <div>
            <button class="btn btn-warning" @click="handleReopenSetupWizard">
              <i class="fas fa-redo me-2"></i>
              {{ $t('troubleshooting.reopen_setup_wizard') }}
            </button>
          </div>
        </div>
      </div>
      
      <!-- Logs -->
      <div class="card p-2 my-4">
        <div class="card-body">
          <h2 id="logs">{{ $t('troubleshooting.logs') }}</h2>
          <br>
          <div>
            <button class="btn btn-primary" @click="handleCopyConfig">{{ $t('troubleshooting.copy_config') }}</button>
          </div>
          <div class="d-flex justify-content-between align-items-baseline py-2">
            <p>{{ $t('troubleshooting.logs_desc') }}</p>
            <input 
              type="text" 
              class="form-control" 
              v-model="logFilter" 
              :placeholder="$t('troubleshooting.logs_find')" 
              style="width: 300px"
            >
          </div>
          <div>
            <div class="troubleshooting-logs">
              <button class="copy-icon" @click="copyLogs">
                <i class="fas fa-copy"></i>
              </button>
              {{ actualLogs }}
            </div>
          </div>
        </div>
      </div>
    </div>
    
    <!-- Confirmation Boom Sunshine Modal -->
    <div id="boomModal" class="modal">
      <div class="modal-content">
        <div class="modal-header">
          <h5>{{ $t('troubleshooting.confirm_boom') }}</h5>
          <span class="close" @click="closeModal">&times;</span>
        </div>
        <div class="modal-body">
          <p>{{ $t('troubleshooting.confirm_boom_desc') }}</p>
        </div>
        <div class="modal-footer">
          <button type="button" class="btn btn-secondary" @click="closeModal">{{ $t('_common.cancel') }}</button>
          <button type="button" class="btn btn-danger" @click="boom">{{ $t('troubleshooting.boom_sunshine') }}</button>
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
.troubleshooting-logs {
  white-space: pre;
  font-family: monospace;
  overflow: auto;
  max-height: 500px;
  min-height: 500px;
  font-size: 16px;
  position: relative;
}

.copy-icon {
  position: absolute;
  top: 8px;
  right: 8px;
  padding: 8px;
  cursor: pointer;
  color: rgba(0, 0, 0, 1);
  appearance: none;
  border: none;
  background: none;
}

.copy-icon:hover {
  color: rgba(0, 0, 0, 0.75);
}

.copy-icon:active {
  color: rgba(0, 0, 0, 1);
}

/* Modal styles */
.modal {
  position: fixed;
  z-index: 1;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
  overflow: auto;
  background-color: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  display: none;
}

.modal.show {
  display: flex;
}

.modal-content {
  background-color: #ffffff !important;
  margin: auto;
  padding: 20px;
  border: 1px solid #ddd;
  max-height: 500px;
  max-width: 500px;
  width: 70% !important;
  border-radius: 8px;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
  position: relative;
}

.modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 8px;
}

.modal-header h5 {
  margin: 0;
  font-size: 1rem;
  font-weight: 800;
}

.close {
  color: #aaa;
  font-size: 24px;
  font-weight: bold;
  cursor: pointer;
  line-height: 1;
  padding: 0 2px;
  position: absolute;
  right: 12px;
  top: 8px;
}

.close:hover,
.close:focus {
  color: black;
  text-decoration: none;
  cursor: pointer;
}

.modal-body {
  margin: 8px 0;
  font-size: 0.95rem;
  line-height: 1.4;
}

.modal-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  margin-top: 8px;
  padding-top: 10px;
  border-top: 1px solid #eee;
}

.modal-footer button {
  padding: 4px 12px;
  font-size: 0.9rem;
}
</style>

