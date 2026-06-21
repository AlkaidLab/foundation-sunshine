<template>
  <div class="page-config">
    <Navbar />
    <div class="config-floating-buttons">
      <button
        class="cute-btn cute-btn-primary"
        :class="{ 'has-unsaved': hasUnsaved }"
        @click="requestConfigAction('save')"
        :disabled="riskActionRunning"
        :title="hasUnsaved ? $t('config.unsaved_changes_tooltip') : $t('_common.save')"
      >
        <i class="fas fa-save"></i>
      </button>
      <button
        v-if="saved && !restarted"
        class="cute-btn cute-btn-success"
        @click="requestConfigAction('apply')"
        :disabled="riskActionRunning"
        :title="$t('_common.apply')"
      >
        <i class="fas fa-check"></i>
      </button>
      <div class="floating-toast-container">
        <Transition name="toast">
          <div
            v-if="showSaveToast"
            class="toast align-items-center text-bg-success border-0 show"
            role="alert"
            aria-live="assertive"
            aria-atomic="true"
          >
            <div class="d-flex">
              <div class="toast-body">
                <i class="fas fa-check-circle me-2"></i>
                <b>{{ $t('_common.success') }}</b> {{ $t('config.apply_note') }}
              </div>
              <button
                type="button"
                class="btn-close btn-close-white me-2 m-auto"
                @click="showSaveToast = false"
                aria-label="Close"
              ></button>
            </div>
          </div>
        </Transition>
        <Transition name="toast">
          <div
            v-if="showRestartToast"
            class="toast align-items-center text-bg-success border-0 mt-2 show"
            role="alert"
            aria-live="assertive"
            aria-atomic="true"
          >
            <div class="d-flex">
              <div class="toast-body">
                <i class="fas fa-check-circle me-2"></i>
                <b>{{ $t('_common.success') }}</b> {{ $t('config.restart_note') }}
              </div>
              <button
                type="button"
                class="btn-close btn-close-white me-2 m-auto"
                @click="showRestartToast = false"
                aria-label="Close"
              ></button>
            </div>
          </div>
        </Transition>
      </div>
    </div>

    <Teleport to="body">
      <Transition name="risk-modal">
        <div v-if="showRiskConfirm" class="risk-confirm-overlay" @click.self="cancelRiskConfirm">
          <div
            ref="riskDialogRef"
            class="risk-confirm-modal"
            role="dialog"
            aria-modal="true"
            aria-labelledby="risk-confirm-title"
            tabindex="-1"
            @keydown.esc.prevent="cancelRiskConfirm"
            @keydown.tab="trapRiskFocus"
          >
            <div class="risk-confirm-header">
              <div>
                <h5 id="risk-confirm-title">
                  <i class="fas fa-exclamation-triangle me-2"></i>
                  {{ $t(riskAction === 'apply' ? 'config.risk_confirm.title_apply' : 'config.risk_confirm.title_save') }}
                </h5>
                <p>
                  {{ $t(riskAction === 'apply' ? 'config.risk_confirm.intro_apply' : 'config.risk_confirm.intro_save') }}
                </p>
              </div>
              <button type="button" class="btn-close" @click="cancelRiskConfirm" :aria-label="$t('_common.close')"></button>
            </div>

            <div class="risk-confirm-body">
              <div
                v-for="risk in riskItems"
                :key="risk.id"
                class="risk-item"
                :class="risk.severity"
              >
                <div class="risk-item-header">
                  <span class="risk-badge" :class="risk.severity">
                    {{ $t(`config.risk_confirm.severity_${risk.severity}`) }}
                  </span>
                  <strong>{{ $t(risk.titleKey) }}</strong>
                </div>
                <p>{{ $t(risk.descriptionKey) }}</p>
                <div v-if="risk.currentValue" class="risk-detail">
                  <span>{{ $t('config.risk_confirm.value_label') }}</span>
                  <code>{{ risk.currentValue }}</code>
                </div>
                <div v-if="risk.recoveryKey" class="risk-recovery">
                  <span>{{ $t('config.risk_confirm.recovery_label') }}</span>
                  <p>{{ $t(risk.recoveryKey) }}</p>
                </div>
              </div>
            </div>

            <div class="risk-confirm-footer">
              <button type="button" class="btn btn-secondary" @click="cancelRiskConfirm" :disabled="riskActionRunning">
                {{ $t('_common.cancel') }}
              </button>
              <button
                type="button"
                class="btn btn-danger"
                @click="confirmRiskAction"
                :disabled="riskActionRunning"
              >
                <i v-if="riskActionRunning" class="fas fa-spinner fa-spin me-1"></i>
                {{ $t(riskAction === 'apply' ? 'config.risk_confirm.confirm_apply' : 'config.risk_confirm.confirm_save') }}
              </button>
            </div>
          </div>
        </div>
      </Transition>
    </Teleport>

    <div class="container">
      <h1 class="my-4 page-title">{{ $t('config.configuration') }}</h1>

      <div v-if="!config" class="form card config-skeleton">
        <div class="card-header skeleton-header">
          <div class="skeleton-tabs">
            <div v-for="n in 6" :key="n" class="skeleton-tab"></div>
          </div>
        </div>
        <div class="config-page skeleton-body">
          <div class="skeleton-section">
            <div class="skeleton-title"></div>
            <div v-for="n in 4" :key="n" class="skeleton-row">
              <div class="skeleton-label"></div>
              <div class="skeleton-input"></div>
            </div>
          </div>
          <div class="skeleton-section">
            <div class="skeleton-title"></div>
            <div v-for="n in 3" :key="n" class="skeleton-row">
              <div class="skeleton-label"></div>
              <div class="skeleton-input"></div>
            </div>
          </div>
        </div>
      </div>

      <div v-else class="form card">
        <ul class="nav nav-tabs config-tabs card-header">
          <template v-for="tab in tabs" :key="tab.id">
            <li
              v-if="tab.type === 'group' && tab.children"
              class="nav-item dropdown"
              :class="{ active: isEncoderTabActive(tab), show: expandedDropdown === tab.id }"
            >
              <a
                class="nav-link dropdown-toggle"
                :class="{ active: isEncoderTabActive(tab) }"
                href="#"
                role="button"
                :aria-expanded="expandedDropdown === tab.id"
                @click.prevent="toggleEncoderDropdown(tab.id, $event)"
              >
                {{ $t(`tabs.${tab.id}`) || tab.name }}
              </a>
              <ul class="dropdown-menu" :class="{ show: expandedDropdown === tab.id }">
                <li v-for="childTab in tab.children" :key="childTab.id">
                  <a
                    class="dropdown-item"
                    :class="[{ active: currentTab === childTab.id }, `encoder-item-${childTab.id}`]"
                    href="#"
                    @click.prevent="selectEncoderTab(childTab.id, $event)"
                  >
                    {{ $t(`tabs.${childTab.id}`) || childTab.name }}
                  </a>
                </li>
              </ul>
            </li>
            <li v-else class="nav-item">
              <a
                class="nav-link"
                :class="{ active: tab.id === currentTab }"
                href="#"
                @click.prevent="currentTab = tab.id"
              >
                {{ $t(`tabs.${tab.id}`) || tab.name }}
              </a>
            </li>
          </template>
        </ul>

        <General
          v-if="currentTab === 'general'"
          :config="config"
          :global-prep-cmd="global_prep_cmd"
          :platform="platform"
        />
        <Inputs v-if="currentTab === 'input'" :config="config" :platform="platform" />
        <AudioVideo
          v-if="currentTab === 'av'"
          :config="config"
          :platform="platform"
          :resolutions="resolutions"
          :fps="fps"
          :display-mode-remapping="display_mode_remapping"
        />
        <Network v-if="currentTab === 'network'" :config="config" :platform="platform" />
        <Files v-if="currentTab === 'files'" :config="config" :platform="platform" />
        <Advanced v-if="currentTab === 'advanced'" :config="config" :platform="platform" />
        <ContainerEncoders
          v-if="isEncoderCurrentTab"
          :current-tab="currentTab"
          :config="config"
          :platform="platform"
        />
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, watch, onMounted, provide, computed, onUnmounted, nextTick, defineAsyncComponent } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import { useConfig } from '../composables/useConfig.js'
import { initFirebase, trackEvents } from '../config/firebase.js'

initFirebase()

const General = defineAsyncComponent(() => import('../configs/tabs/General.vue'))
const Inputs = defineAsyncComponent(() => import('../configs/tabs/Inputs.vue'))
const Network = defineAsyncComponent(() => import('../configs/tabs/Network.vue'))
const Files = defineAsyncComponent(() => import('../configs/tabs/Files.vue'))
const Advanced = defineAsyncComponent(() => import('../configs/tabs/Advanced.vue'))
const AudioVideo = defineAsyncComponent(() => import('../configs/tabs/AudioVideo.vue'))
const ContainerEncoders = defineAsyncComponent(() => import('../configs/tabs/ContainerEncoders.vue'))

const ENCODER_TAB_IDS = new Set(['nv', 'qsv', 'amd', 'vt', 'sw'])

const {
  platform,
  saved,
  restarted,
  config,
  fps,
  resolutions,
  currentTab,
  global_prep_cmd,
  display_mode_remapping,
  tabs,
  initTabs,
  loadConfig,
  save: saveConfig,
  apply: applyConfig,
  getRiskyChanges,
  handleHash,
  hasUnsavedChanges,
} = useConfig()

const showSaveToast = ref(false)
const showRestartToast = ref(false)
const expandedDropdown = ref(null)
const showRiskConfirm = ref(false)
const riskAction = ref('save')
const riskItems = ref([])
const riskActionRunning = ref(false)
const riskDialogRef = ref(null)
const lastFocusedElement = ref(null)

const hasUnsaved = computed(() => {
  if (!config.value) return false
  void config.value
  void fps.value
  void resolutions.value
  void global_prep_cmd.value
  void display_mode_remapping.value
  return hasUnsavedChanges()
})

const isEncoderCurrentTab = computed(() => ENCODER_TAB_IDS.has(currentTab.value))

const isEncoderTabActive = (tab) => tab.type === 'group' && tab.children?.some((child) => child.id === currentTab.value)

const toggleEncoderDropdown = (tabId, event) => {
  event.stopPropagation()

  if (expandedDropdown.value === tabId) {
    expandedDropdown.value = null
    return
  }

  expandedDropdown.value = tabId

  const encoderGroup = tabs.value.find((t) => t.id === tabId && t.type === 'group')
  const children = encoderGroup?.children

  if (children?.length && !children.some((child) => child.id === currentTab.value)) {
    currentTab.value = children[0].id
  }
}

const selectEncoderTab = (childTabId, event) => {
  event.stopPropagation()
  currentTab.value = childTabId
  expandedDropdown.value = null
}

const showToast = (toastRef, duration = 5000) => {
  toastRef.value = true
  setTimeout(() => {
    toastRef.value = false
  }, duration)
}

const getRiskFocusableElements = () => {
  const root = riskDialogRef.value
  if (!root) return []

  return Array.from(
    root.querySelectorAll(
      [
        'button:not([disabled])',
        '[href]',
        'input:not([disabled])',
        'select:not([disabled])',
        'textarea:not([disabled])',
        '[tabindex]:not([tabindex="-1"])',
      ].join(','),
    ),
  ).filter((element) => element.offsetParent !== null)
}

const focusRiskDialog = async () => {
  await nextTick()
  const focusable = getRiskFocusableElements()
  const target = focusable[0] || riskDialogRef.value
  target?.focus?.()
}

const restoreRiskFocus = () => {
  const target = lastFocusedElement.value
  lastFocusedElement.value = null

  if (target && document.contains(target)) {
    target.focus?.()
  }
}

const trapRiskFocus = (event) => {
  const focusable = getRiskFocusableElements()
  if (!focusable.length) {
    event.preventDefault()
    riskDialogRef.value?.focus?.()
    return
  }

  const currentIndex = focusable.indexOf(document.activeElement)
  const lastIndex = focusable.length - 1
  let nextIndex = currentIndex + 1

  if (event.shiftKey) {
    nextIndex = currentIndex <= 0 ? lastIndex : currentIndex - 1
  } else if (currentIndex === -1 || currentIndex >= lastIndex) {
    nextIndex = 0
  }

  event.preventDefault()
  focusable[nextIndex].focus()
}

const runConfigAction = async (action) => {
  if (riskActionRunning.value) return

  riskActionRunning.value = true
  try {
    if (action === 'apply') {
      await applyConfig()
    } else {
      await saveConfig()
    }
  } finally {
    riskActionRunning.value = false
  }
}

const requestConfigAction = async (action) => {
  if (riskActionRunning.value || showRiskConfirm.value) return

  const risks = getRiskyChanges(action)
  if (risks.length > 0) {
    lastFocusedElement.value = document.activeElement
    riskAction.value = action
    riskItems.value = risks
    showRiskConfirm.value = true
    return
  }

  await runConfigAction(action)
}

const cancelRiskConfirm = () => {
  if (riskActionRunning.value) return
  showRiskConfirm.value = false
  riskItems.value = []
}

const confirmRiskAction = async () => {
  if (riskActionRunning.value) return

  const action = riskAction.value
  showRiskConfirm.value = false
  await runConfigAction(action)
  riskItems.value = []
}

watch(showRiskConfirm, async (isOpen) => {
  if (isOpen) {
    await focusRiskDialog()
    return
  }

  restoreRiskFocus()
})

watch(saved, (newVal) => {
  if (newVal && !restarted.value) {
    showToast(showSaveToast)
  }
})

watch(restarted, (newVal) => {
  if (newVal) {
    showSaveToast.value = false
    showToast(showRestartToast)
  }
})

provide(
  'platform',
  computed(() => platform.value)
)

const handleOutsideClick = (event) => {
  if (expandedDropdown.value && !event.target.closest('.dropdown')) {
    expandedDropdown.value = null
  }
}

onMounted(async () => {
  trackEvents.pageView('configuration')
  initTabs()
  await loadConfig()
  handleHash()

  window.addEventListener('hashchange', handleHash)
  document.addEventListener('click', handleOutsideClick)
})

onUnmounted(() => {
  window.removeEventListener('hashchange', handleHash)
  document.removeEventListener('click', handleOutsideClick)
  lastFocusedElement.value = null
})
</script>

<style lang="less">
@import '../styles/global.less';

// Variables
@transition-fast: 0.3s;
@border-radius-sm: 2px;
@border-radius-md: 10px;
@border-radius-lg: 12px;
@btn-size: 56px;
@btn-size-mobile: 48px;
@cubic-bounce: cubic-bezier(0.68, -0.55, 0.265, 1.55);
@cubic-smooth: cubic-bezier(0.4, 0, 0.2, 1);

// Encoder brand colors
@color-nvidia: #76b900;
@color-amd: #ed1c24;
@color-intel: #0071c5;

// Mixins
.flex-center() {
  display: flex;
  justify-content: center;
  align-items: center;
}

.transition(@properties: all) {
  transition: @properties @transition-fast @cubic-smooth;
}

.skeleton-gradient(@light: 0.08, @mid: 0.12) {
  background: linear-gradient(90deg, rgba(0, 0, 0, @light) 25%, rgba(0, 0, 0, @mid) 50%, rgba(0, 0, 0, @light) 75%);
  background-size: 200% 100%;
  animation: skeleton-shimmer 1.5s infinite;
}

.config-page {
  padding: 1em;
  border: 1px solid transparent;
  border-top: none;
}

.config-skeleton {
  .skeleton-header {
    background: linear-gradient(135deg, rgba(255, 255, 255, 0.2), rgba(255, 255, 255, 0.1));
    border-radius: @border-radius-lg @border-radius-lg 0 0;
    padding: 0.5rem 1rem;
  }

  .skeleton-tabs {
    display: flex;
    gap: 0.5rem;
    padding: 0.5rem 0;
  }

  .skeleton-tab {
    width: 80px;
    height: 38px;
    .skeleton-gradient();
    border-radius: @border-radius-md;
  }

  .skeleton-body {
    padding: 1.5rem;
  }

  .skeleton-section {
    margin-bottom: 2rem;
    &:last-child {
      margin-bottom: 0;
    }
  }

  .skeleton-title {
    width: 150px;
    height: 24px;
    .skeleton-gradient();
    border-radius: 4px;
    margin-bottom: 1rem;
  }

  .skeleton-row {
    display: flex;
    align-items: center;
    gap: 1rem;
    margin-bottom: 1rem;
    &:last-child {
      margin-bottom: 0;
    }
  }

  .skeleton-label {
    width: 120px;
    height: 16px;
    .skeleton-gradient(0.06, 0.1);
    border-radius: 4px;
    flex-shrink: 0;
  }

  .skeleton-input {
    flex: 1;
    height: 38px;
    .skeleton-gradient(0.06, 0.1);
    border-radius: 6px;
    max-width: 300px;
  }
}

@keyframes skeleton-shimmer {
  0% {
    background-position: 200% 0;
  }
  100% {
    background-position: -200% 0;
  }
}

[data-bs-theme='dark'] .config-skeleton {
  .skeleton-header {
    background: linear-gradient(135deg, rgba(255, 255, 255, 0.1), rgba(255, 255, 255, 0.05));
  }

  .skeleton-tab,
  .skeleton-title,
  .skeleton-label,
  .skeleton-input {
    background: linear-gradient(
      90deg,
      rgba(255, 255, 255, 0.06) 25%,
      rgba(255, 255, 255, 0.1) 50%,
      rgba(255, 255, 255, 0.06) 75%
    );
    background-size: 200% 100%;
    animation: skeleton-shimmer 1.5s infinite;
  }
}

.page-config {
  .nav-tabs {
    border: none;
  }

  .ms-item {
    border: 1px solid;
    border-radius: @border-radius-sm;
    font-size: 12px;
    font-weight: bold;
  }

  .config-tabs {
    background: linear-gradient(135deg, rgba(255, 255, 255, 0.2), rgba(255, 255, 255, 0.1));
    border-radius: @border-radius-lg @border-radius-lg 0 0;
    padding: 0.5rem 1rem 0;
    gap: 0.5rem;
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);
    position: relative;
    z-index: 10;
    overflow: visible;

    .nav-item {
      margin-bottom: -1px;

      &.dropdown {
        position: relative;
        &.show .dropdown-menu {
          display: block;
        }
      }
    }

    .nav-link {
      border: none;
      border-radius: @border-radius-md @border-radius-md 0 0;
      padding: 0.75rem 1.5rem;
      font-weight: 500;
      color: var(--bs-secondary-color);
      background: transparent;
      position: relative;
      overflow: hidden;
      .transition();

      &::before {
        content: '';
        position: absolute;
        bottom: 0;
        left: 50%;
        transform: translateX(-50%) scaleX(0);
        width: 80%;
        height: 3px;
        background: linear-gradient(90deg, var(--bs-primary), var(--bs-info));
        border-radius: 3px 3px 0 0;
        .transition(transform);
      }

      &:hover {
        color: var(--bs-primary);
        background: rgba(var(--bs-primary-rgb), 0.08);
      }

      &.active {
        color: var(--bs-primary);
        background: var(--bs-body-bg);
        box-shadow: 0 -4px 12px rgba(0, 0, 0, 0.08);
        font-weight: 600;

        &::before {
          transform: translateX(-50%) scaleX(1);
        }
      }

      &.dropdown-toggle::after {
        margin-left: 0.5em;
        .transition(transform);
      }

      &.dropdown-toggle[aria-expanded='true']::after {
        transform: rotate(180deg);
      }
    }

    .dropdown-menu {
      display: none;
      position: absolute;
      top: 100%;
      left: 0;
      z-index: 1050;
      min-width: 200px;
      margin-top: 0.25rem;
      padding: 0.5rem 0;
      border-radius: @border-radius-md;
      border: 1px solid rgba(0, 0, 0, 0.1);
      box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
      background: rgba(var(--bs-body-bg-rgb), 0.95);
      backdrop-filter: blur(10px);

      &.show {
        display: block;
      }

      .dropdown-item {
        display: flex;
        align-items: center;
        padding: 0.5rem 1.5rem;
        font-weight: 500;
        text-decoration: none;
        .transition();

        &.encoder-item-nv {
          color: @color-nvidia;
        }
        &.encoder-item-amd {
          color: @color-amd;
        }
        &.encoder-item-qsv {
          color: @color-intel;
        }
        &.encoder-item-sw {
          color: var(--bs-secondary-color);
        }

        &:hover {
          background: rgba(var(--bs-primary-rgb), 0.08);
        }
        &.active {
          background: rgba(var(--bs-primary-rgb), 0.15);
          font-weight: 600;
        }
      }
    }
  }
}

// Toast transitions
.toast-enter-active,
.toast-leave-active {
  transition: opacity @transition-fast ease-in-out;
}

.toast-enter-from,
.toast-leave-to {
  opacity: 0;
}

.toast.show {
  opacity: 1;
}

.risk-modal-enter-active,
.risk-modal-leave-active {
  transition: opacity @transition-fast ease;

  .risk-confirm-modal {
    transition: transform @transition-fast @cubic-smooth, opacity @transition-fast ease;
  }
}

.risk-modal-enter-from,
.risk-modal-leave-to {
  opacity: 0;

  .risk-confirm-modal {
    opacity: 0;
    transform: translateY(16px) scale(0.98);
  }
}

.risk-confirm-overlay {
  position: fixed;
  inset: 0;
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 1.5rem);
  background: var(--overlay-bg, rgba(0, 0, 0, 0.7));
  backdrop-filter: blur(8px);

  [data-bs-theme='light'] & {
    background: rgba(0, 0, 0, 0.5);
  }
}

.risk-confirm-modal {
  width: min(720px, 100%);
  max-height: min(760px, calc(100vh - 2.5rem));
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border-radius: var(--border-radius-xl, 1.5rem);
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.2));
  background: var(--modal-bg, rgba(30, 30, 50, 0.95));
  backdrop-filter: blur(20px);
  box-shadow: var(--shadow-xl, 0 25px 50px rgba(0, 0, 0, 0.5));
  color: var(--text-primary, #fff);

  [data-bs-theme='light'] & {
    border-color: rgba(0, 0, 0, 0.15);
    background: rgba(255, 255, 255, 0.95);
    box-shadow: 0 25px 50px rgba(0, 0, 0, 0.2);
    color: #000;
  }
}

.risk-confirm-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
  padding: var(--spacing-md, 1rem) var(--spacing-lg, 1.5rem);
  border-bottom: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  h5 {
    margin: 0 0 0.35rem;
    font-size: var(--font-size-lg, 1.25rem);
    font-weight: 600;
    color: var(--text-primary, #fff);

    i {
      color: var(--bs-danger);
    }
  }

  p {
    margin: 0;
    color: var(--text-secondary, rgba(255, 255, 255, 0.85));
    line-height: 1.5;
  }

  .btn-close {
    flex: 0 0 auto;
  }

  [data-bs-theme='light'] & {
    border-bottom-color: rgba(0, 0, 0, 0.1);

    h5 {
      color: #000;
    }

    p {
      color: #4b5563;
    }
  }
}

.risk-confirm-body {
  overflow-y: auto;
  padding: var(--spacing-md, 1rem) var(--spacing-lg, 1.5rem);
}

.risk-item {
  padding: 1rem;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: @border-radius-md;
  background: rgba(255, 255, 255, 0.06);

  & + & {
    margin-top: 0.75rem;
  }

  &.critical {
    border-color: rgba(var(--bs-danger-rgb), 0.38);
    background: rgba(var(--bs-danger-rgb), 0.1);
  }

  &.high {
    border-color: rgba(var(--bs-warning-rgb), 0.36);
  }

  &.medium {
    border-color: rgba(var(--bs-info-rgb), 0.32);
  }

  p {
    margin: 0.5rem 0 0;
    color: var(--text-primary, #fff);
    line-height: 1.5;
  }

  [data-bs-theme='light'] & {
    border-color: rgba(0, 0, 0, 0.1);
    background: rgba(255, 255, 255, 0.75);

    &.critical {
      border-color: rgba(var(--bs-danger-rgb), 0.34);
      background: rgba(var(--bs-danger-rgb), 0.08);
    }

    &.high {
      border-color: rgba(var(--bs-warning-rgb), 0.4);
      background: rgba(var(--bs-warning-rgb), 0.08);
    }

    &.medium {
      border-color: rgba(var(--bs-info-rgb), 0.32);
      background: rgba(var(--bs-info-rgb), 0.06);
    }

    p {
      color: #000;
    }
  }
}

.risk-item-header {
  display: flex;
  align-items: center;
  gap: 0.6rem;
  flex-wrap: wrap;

  strong {
    font-size: 0.98rem;
  }
}

.risk-badge {
  display: inline-flex;
  align-items: center;
  min-height: 24px;
  padding: 0.15rem 0.55rem;
  border-radius: 999px;
  font-size: 0.75rem;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.02em;

  &.critical {
    color: #fff;
    background: var(--bs-danger);
  }

  &.high {
    color: #231f20;
    background: var(--bs-warning);
  }

  &.medium {
    color: #fff;
    background: var(--bs-info);
  }
}

.risk-detail,
.risk-recovery {
  margin-top: 0.7rem;
  padding-top: 0.7rem;
  border-top: 1px solid rgba(255, 255, 255, 0.12);

  span {
    display: block;
    margin-bottom: 0.25rem;
    color: var(--text-secondary, rgba(255, 255, 255, 0.85));
    font-size: 0.8rem;
    font-weight: 600;
  }

  code {
    display: inline-block;
    max-width: 100%;
    overflow-wrap: anywhere;
    padding: 0.2rem 0.4rem;
    border-radius: @border-radius-sm;
    color: var(--text-primary, #fff);
    background: rgba(255, 255, 255, 0.1);
  }

  [data-bs-theme='light'] & {
    border-top-color: rgba(0, 0, 0, 0.1);

    span {
      color: #4b5563;
    }

    code {
      color: #000;
      background: rgba(0, 0, 0, 0.06);
    }
  }
}

.risk-recovery p {
  margin: 0;
  color: var(--text-secondary, rgba(255, 255, 255, 0.85));

  [data-bs-theme='light'] & {
    color: #4b5563;
  }
}

.risk-confirm-footer {
  display: flex;
  justify-content: flex-end;
  gap: 0.75rem;
  padding: var(--spacing-md, 1rem) var(--spacing-lg, 1.5rem);
  border-top: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  [data-bs-theme='light'] & {
    border-top-color: rgba(0, 0, 0, 0.1);
  }
}

.config-floating-buttons {
  position: sticky;
  top: 80%;
  right: 2rem;
  float: right;
  clear: right;
  margin: 2rem 0;
  display: flex;
  flex-direction: column;
  gap: 1rem;
  z-index: 1000;

  .floating-toast-container {
    position: absolute;
    right: calc(100% + 1rem);
    top: 0;
    width: max-content;
    max-width: 300px;

    .toast {
      margin-bottom: 0.5rem;
    }
  }

  .cute-btn {
    width: @btn-size;
    height: @btn-size;
    border-radius: 50%;
    border: 3px solid rgba(255, 255, 255, 0.4);
    color: #fff;
    font-size: 1.25rem;
    cursor: pointer;
    position: relative;
    overflow: hidden;
    .transition();
    .flex-center();

    &::before {
      content: '';
      position: absolute;
      top: -50%;
      left: -50%;
      width: 200%;
      height: 200%;
      background: radial-gradient(circle, rgba(255, 255, 255, 0.3) 0%, transparent 70%);
      opacity: 0;
      .transition(opacity);
    }

    &::after {
      content: '';
      position: absolute;
      top: 20%;
      left: 20%;
      width: 30%;
      height: 30%;
      background: radial-gradient(circle, rgba(255, 255, 255, 0.6) 0%, transparent 70%);
      border-radius: 50%;
      opacity: 0.8;
    }

    &:hover {
      transform: scale(1.1) translateY(-2px);
      box-shadow: 0 8px 20px rgba(0, 0, 0, 0.2), 0 0 30px rgba(255, 255, 255, 0.3);

      &::before,
      &::after {
        opacity: 1;
      }
    }

    &:active {
      transform: scale(0.95) translateY(0);
      transition: transform 0.1s @cubic-bounce;
    }

    &-primary {
      background: linear-gradient(135deg, #ff6b9d, #c44569, #f093fb);
      background-size: 200% 200%;
      box-shadow: 0 4px 15px rgba(255, 107, 157, 0.4), 0 0 20px rgba(255, 107, 157, 0.2),
        inset 0 1px 0 rgba(255, 255, 255, 0.3);

      &:hover {
        animation: gradient-shift 1.5s ease infinite;
        box-shadow: 0 8px 25px rgba(255, 107, 157, 0.6), 0 0 40px rgba(255, 107, 157, 0.4),
          inset 0 1px 0 rgba(255, 255, 255, 0.4);
      }

      &.has-unsaved {
        animation: pulse-warning 2s ease-in-out 3;
        box-shadow: 0 4px 15px rgba(255, 107, 157, 0.4), 0 0 20px rgba(255, 107, 157, 0.2),
          0 0 0 3px rgba(255, 193, 7, 0.5), inset 0 1px 0 rgba(255, 255, 255, 0.3);

        &:hover {
          animation: gradient-shift 1.5s ease infinite, pulse-warning 2s ease-in-out 3;
          box-shadow: 0 8px 25px rgba(255, 107, 157, 0.6), 0 0 40px rgba(255, 107, 157, 0.4),
            0 0 0 4px rgba(255, 193, 7, 0.7), inset 0 1px 0 rgba(255, 255, 255, 0.4);
        }
      }
    }

    &-success {
      background: linear-gradient(135deg, #4facfe, #00f2fe, #43e97b);
      background-size: 200% 200%;
      box-shadow: 0 4px 15px rgba(79, 172, 254, 0.4), 0 0 20px rgba(79, 172, 254, 0.2),
        inset 0 1px 0 rgba(255, 255, 255, 0.3);

      &:hover {
        animation: gradient-shift 1.5s ease infinite;
        box-shadow: 0 8px 25px rgba(79, 172, 254, 0.6), 0 0 40px rgba(79, 172, 254, 0.4),
          inset 0 1px 0 rgba(255, 255, 255, 0.4);
      }
    }

    i {
      position: relative;
      z-index: 1;
      .transition(transform);
    }

    &:hover i {
      transform: scale(1.2) rotate(5deg);
    }

    &:disabled {
      cursor: not-allowed;
      opacity: 0.65;
      transform: none;
      box-shadow: none;
      animation: none;

      &::before {
        opacity: 0;
      }

      i {
        transform: none;
      }
    }
  }

  @keyframes gradient-shift {
    0%,
    100% {
      background-position: 0% 50%;
    }
    50% {
      background-position: 100% 50%;
    }
  }

  @keyframes pulse-warning {
    0%,
    100% {
      box-shadow: 0 4px 15px rgba(255, 107, 157, 0.4), 0 0 20px rgba(255, 107, 157, 0.2),
        0 0 0 3px rgba(255, 193, 7, 0.5), inset 0 1px 0 rgba(255, 255, 255, 0.3);
    }
    50% {
      box-shadow: 0 4px 15px rgba(255, 107, 157, 0.4), 0 0 20px rgba(255, 107, 157, 0.2),
        0 0 0 5px rgba(255, 193, 7, 0.8), inset 0 1px 0 rgba(255, 255, 255, 0.3);
    }
  }
}

// Dark mode
[data-bs-theme='dark'] .page-config .config-tabs {
  background: linear-gradient(135deg, rgba(255, 255, 255, 0.1), rgba(255, 255, 255, 0.05));
  border-bottom-color: rgba(255, 255, 255, 0.1);

  .nav-link {
    &:hover {
      background: rgba(var(--bs-primary-rgb), 0.15);
    }
    &.active {
      box-shadow: 0 -4px 12px rgba(0, 0, 0, 0.3);
    }
  }

  .dropdown-menu {
    border-color: rgba(255, 255, 255, 0.1);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);

    .dropdown-item {
      &:hover {
        background: rgba(var(--bs-primary-rgb), 0.15);
      }
      &.active {
        background: rgba(var(--bs-primary-rgb), 0.25);
      }
    }
  }
}

[data-bs-theme='dark'] .config-floating-buttons .cute-btn {
  border-color: rgba(255, 255, 255, 0.5);

  &-primary {
    box-shadow: 0 4px 15px rgba(255, 107, 157, 0.5), 0 0 25px rgba(255, 107, 157, 0.3),
      inset 0 1px 0 rgba(255, 255, 255, 0.4);

    &:hover {
      box-shadow: 0 8px 25px rgba(255, 107, 157, 0.7), 0 0 50px rgba(255, 107, 157, 0.5),
        inset 0 1px 0 rgba(255, 255, 255, 0.5);
    }
  }

  &-success {
    box-shadow: 0 4px 15px rgba(79, 172, 254, 0.5), 0 0 25px rgba(79, 172, 254, 0.3),
      inset 0 1px 0 rgba(255, 255, 255, 0.4);

    &:hover {
      box-shadow: 0 8px 25px rgba(79, 172, 254, 0.7), 0 0 50px rgba(79, 172, 254, 0.5),
        inset 0 1px 0 rgba(255, 255, 255, 0.5);
    }
  }
}

// Responsive
@media (max-width: 768px) {
  .config-floating-buttons {
    position: fixed;
    right: 1rem;
    bottom: 1rem;
    top: auto;
    float: none;
    margin: 0;
    gap: 0.75rem;

    .floating-toast-container {
      right: auto;
      left: auto;
      top: auto;
      bottom: calc(100% + 1rem);
      max-width: calc(100vw - 2rem);
    }

    .cute-btn {
      width: @btn-size-mobile;
      height: @btn-size-mobile;
      font-size: 1.1rem;
    }
  }

  .page-config .config-tabs {
    padding: 0.5rem 0.5rem 0;
    gap: 0.25rem;
    overflow-x: auto;
    flex-wrap: nowrap;

    .nav-link {
      padding: 0.5rem 1rem;
      font-size: 0.875rem;
      white-space: nowrap;
    }
  }
}

// 无障碍：减少动态效果
@media (prefers-reduced-motion: reduce) {
  .config-floating-buttons .cute-btn {
    animation: none !important;
    transition: none !important;

    &:hover {
      animation: none !important;
    }
  }

  .config-skeleton .skeleton-tab,
  .config-skeleton .skeleton-title,
  .config-skeleton .skeleton-label,
  .config-skeleton .skeleton-input {
    animation: none !important;
  }
}
</style>
