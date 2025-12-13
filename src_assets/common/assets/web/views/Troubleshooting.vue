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
            <button class="btn btn-danger" :disabled="boomPressed" @click="showBoomModal">
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

    <!-- Boom Confirm Modal -->
    <Transition name="fade">
      <div v-if="showBoomConfirmModal" class="boom-confirm-overlay" @click.self="closeBoomModal">
        <div class="boom-confirm-modal">
          <div class="boom-confirm-header">
            <h5>
              <i class="fas fa-bomb me-2"></i>{{ $t('troubleshooting.confirm_boom') }}
            </h5>
            <button class="btn-close" @click="closeBoomModal"></button>
          </div>
          <div class="boom-confirm-body">
            <p>{{ $t('troubleshooting.confirm_boom_desc') }}</p>
          </div>
          <div class="boom-confirm-footer">
            <button type="button" class="btn btn-secondary" @click="closeBoomModal">
              {{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-danger" @click="confirmBoom">
              <i class="fas fa-bomb me-2"></i>{{ $t('troubleshooting.boom_sunshine') }}
            </button>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>

<script setup>
import { onMounted, ref } from 'vue'
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

const showBoomConfirmModal = ref(false)

const showBoomModal = () => {
  showBoomConfirmModal.value = true
}

const closeBoomModal = () => {
  showBoomConfirmModal.value = false
}

const confirmBoom = () => {
  closeBoomModal()
  boom()
}

const handleCopyConfig = () => copyConfig(t)

const handleReopenSetupWizard = () => reopenSetupWizard(t)

onMounted(async () => {
  await Promise.all([loadPlatform(), refreshLogs()])
  startLogRefresh()
})
</script>

<style>
@import '../styles/global.less';
</style>

<style scoped>
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

/* Boom Confirm Modal - 使用 ScanResultModal 样式 */
.boom-confirm-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  width: 100vw;
  height: 100vh;
  margin: 0;
  background: var(--overlay-bg, rgba(0, 0, 0, 0.7));
  backdrop-filter: blur(8px);
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 20px);
  overflow: hidden;
  
  [data-bs-theme='light'] & {
    background: rgba(0, 0, 0, 0.5);
  }
}

.boom-confirm-modal {
  background: var(--modal-bg, rgba(30, 30, 50, 0.95));
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.2));
  border-radius: var(--border-radius-xl, 12px);
  width: 100%;
  max-width: 500px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  backdrop-filter: blur(20px);
  box-shadow: var(--shadow-xl, 0 25px 50px rgba(0, 0, 0, 0.5));
  animation: modalSlideUp 0.3s ease;
  
  [data-bs-theme='light'] & {
    background: rgba(255, 255, 255, 0.95);
    border: 1px solid rgba(0, 0, 0, 0.15);
    box-shadow: 0 25px 50px rgba(0, 0, 0, 0.2);
  }
}

@keyframes modalSlideUp {
  from {
    transform: translateY(20px);
    opacity: 0;
  }
  to {
    transform: translateY(0);
    opacity: 1;
  }
}

.boom-confirm-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-bottom: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  h5 {
    margin: 0;
    color: var(--text-primary, #fff);
    font-size: var(--font-size-lg, 1.1rem);
    font-weight: 600;
    display: flex;
    align-items: center;
    gap: var(--spacing-sm, 8px);
  }
  
  [data-bs-theme='light'] & {
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);
    
    h5 {
      color: #000000;
    }
  }
}

.boom-confirm-body {
  padding: var(--spacing-lg, 24px);
  font-size: var(--font-size-md, 0.95rem);
  line-height: 1.5;
  overflow-y: auto;
  flex: 1;
  color: var(--text-primary, #fff);
  
  [data-bs-theme='light'] & {
    color: #000000;
  }
}

.boom-confirm-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-top: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));
  
  [data-bs-theme='light'] & {
    border-top: 1px solid rgba(0, 0, 0, 0.1);
  }
}

.boom-confirm-footer button {
  padding: 8px 16px;
  font-size: 0.9rem;
}

/* Vue 过渡动画 */
.fade-enter-active {
  transition: opacity 0.3s ease;
}

.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

@media (max-width: 991.98px) {
  .page-title {
    font-size: 1.5rem;
  }
}
</style>
