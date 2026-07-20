<template>
  <div class="page-config">
    <Navbar />
    <div class="config-floating-buttons">
      <button
        type="button"
        class="cute-btn cute-btn-primary"
        :class="{ 'has-unsaved': hasUnsaved }"
        @click="requestConfigAction('save')"
        :disabled="riskActionRunning"
        :aria-label="$t('_common.save')"
        :title="hasUnsaved ? $t('config.unsaved_changes_tooltip') : $t('_common.save')"
      >
        <i class="fas fa-save"></i>
      </button>
      <button
        v-if="saved && !restarted"
        type="button"
        class="cute-btn cute-btn-primary"
        @click="requestConfigAction('apply')"
        :disabled="riskActionRunning"
        :aria-label="$t('_common.apply')"
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

    <ConfirmDialog
      :show="showRiskConfirm"
      dialog-id="risk-confirm"
      :title="$t(riskAction === 'apply' ? 'config.risk_confirm.title_apply' : 'config.risk_confirm.title_save')"
      title-icon="fas fa-exclamation-triangle"
      tone="danger"
      max-width="720px"
      :close-label="$t('_common.close')"
      @close="cancelRiskConfirm"
    >
      <p class="risk-confirm-intro">
        {{ $t(riskAction === 'apply' ? 'config.risk_confirm.intro_apply' : 'config.risk_confirm.intro_save') }}
      </p>
      <div class="risk-confirm-list">
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

      <template #actions>
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
      </template>
    </ConfirmDialog>

    <div class="container">
      <h1 class="mt-2 mb-4 page-title">{{ $t('config.configuration') }}</h1>

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
        <div class="config-tabs-shell">
          <ul ref="configTabsRef" class="nav nav-tabs config-tabs">
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
        </div>

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
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import { getPreferredEncoderTab } from '../config/encoderTabs.js'
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
const configTabsRef = ref(null)

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

const preferredEncoderTab = (children = []) =>
  getPreferredEncoderTab({
    activeEncoder: config.value?.active_encoder,
    configuredEncoder: config.value?.encoder,
    selectedAdapter: config.value?.adapter_name,
    adapters: config.value?.adapters,
    platform: platform.value,
    availableTabIds: children.map((child) => child.id),
  })

const scrollActiveTabIntoView = async () => {
  await nextTick()
  const activeTab = configTabsRef.value?.querySelector('.nav-link.active')
  const behavior = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth'
  activeTab?.scrollIntoView?.({ behavior, block: 'nearest', inline: 'center' })
}

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
    currentTab.value = preferredEncoderTab(children)
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

watch(currentTab, scrollActiveTabIntoView)

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

const handleConfigHash = () => {
  handleHash()

  if (currentTab.value === 'encoders') {
    const encoderGroup = tabs.value.find((tab) => tab.id === 'encoders')
    currentTab.value = preferredEncoderTab(encoderGroup?.children)
  }
}

onMounted(async () => {
  trackEvents.pageView('configuration')
  initTabs()
  await loadConfig()
  handleConfigHash()

  await scrollActiveTabIntoView()

  window.addEventListener('hashchange', handleConfigHash)
  document.addEventListener('click', handleOutsideClick)
})

onUnmounted(() => {
  window.removeEventListener('hashchange', handleConfigHash)
  document.removeEventListener('click', handleOutsideClick)
})
</script>

<style lang="less">
@import '../styles/global.less';

// Variables
@transition-fast: 0.3s;
@border-radius-sm: 2px;
@border-radius-md: 10px;
@border-radius-lg: 12px;
@btn-size: 52px;
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

.skeleton-gradient() {
  background: linear-gradient(
    90deg,
    var(--ui-skeleton-base) 25%,
    var(--ui-skeleton-highlight) 50%,
    var(--ui-skeleton-base) 75%
  );
  background-size: 200% 100%;
  animation: skeleton-shimmer 1.5s infinite;
}

.config-page {
  padding: 1em;
  border: 1px solid var(--ui-border);
  border-top: none;
  border-radius: 0 0 var(--ui-radius-md) var(--ui-radius-md);
  background: var(--ui-surface-strong);
  color: var(--ui-text-primary);
}

.config-skeleton {
  .skeleton-header {
    background: var(--ui-surface-strong);
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
    .skeleton-gradient();
    border-radius: 4px;
    flex-shrink: 0;
  }

  .skeleton-input {
    flex: 1;
    height: 38px;
    .skeleton-gradient();
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

.page-config {
  min-height: 100vh;
  padding-bottom: var(--spacing-xl);
  background: linear-gradient(180deg, rgba(var(--ui-accent-rgb), 0.06), transparent 28rem);

  .page-title {
    color: var(--ui-text-primary) !important;
    font-weight: 600;
  }

  .form.card {
    overflow: visible;
    border: 1px solid var(--ui-border);
    border-radius: var(--ui-radius-md);
    background: var(--ui-surface);
    box-shadow: var(--ui-shadow-sm);
  }

  .config-page {
    .accordion-item {
      overflow: hidden;
      border: 1px solid var(--ui-border);
      border-radius: var(--ui-radius-md);
      background: var(--ui-surface);
      box-shadow: var(--ui-shadow-sm);
    }

    .accordion-button {
      border: 0;
      background: var(--ui-surface-strong);
      color: var(--ui-text-primary);
      font-weight: 600;
      transition: color 0.2s ease, background-color 0.2s ease;

      &:hover,
      &:not(.collapsed) {
        background: var(--ui-accent-soft);
        color: var(--ui-accent);
      }

      &:focus {
        box-shadow: inset 0 0 0 2px var(--ui-accent-soft);
      }
    }

    .accordion-body {
      background: transparent;
    }

    pre {
      max-width: 100%;
      margin: 0.5rem 0;
      padding: 0.75rem 1rem;
      overflow: auto;
      border: 1px solid var(--ui-border);
      border-radius: var(--ui-radius-sm);
      background: var(--ui-surface);
      color: var(--ui-text-secondary);
      font-size: 0.82rem;
    }

    .pre-line {
      white-space: pre-line;
    }

    .settings-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 0.85rem;
    }

    .settings-field,
    .settings-panel {
      min-width: 0;
      padding: 0.9rem;
      border: 1px solid var(--ui-border);
      border-radius: var(--ui-radius-md);
      background: var(--ui-surface);
      box-shadow: var(--ui-shadow-sm);
    }

    .settings-panel--accent {
      border-left: 3px solid var(--ui-accent);
      background: color-mix(in srgb, var(--ui-accent) 5%, var(--ui-surface));
    }

    .settings-subpanel {
      padding: 0.8rem;
      border: 1px solid var(--ui-border);
      border-left: 3px solid var(--ui-border-strong);
      border-radius: var(--ui-radius-sm);
      background: var(--ui-surface-strong);
    }

    .settings-toggle-field {
      display: flex;
      flex-direction: column;
      justify-content: center;
    }

    .settings-field > .form-label:first-child,
    .settings-panel > .form-label:first-child {
      color: var(--ui-text-primary);
      font-weight: 600;
    }

    @media (max-width: 767.98px) {
      .settings-grid {
        grid-template-columns: minmax(0, 1fr);
      }

      .settings-field,
      .settings-panel {
        padding: 0.75rem;
      }
    }
  }

  .nav-tabs {
    border: none;
  }

  .ms-item {
    border: 1px solid;
    border-radius: @border-radius-sm;
    font-size: 12px;
    font-weight: bold;
  }

  .config-tabs-shell {
    position: relative;
    border-radius: @border-radius-lg @border-radius-lg 0 0;
    z-index: 10;
    background: var(--ui-surface-strong);
  }

  .config-tabs {
    background: var(--ui-surface-strong);
    border-radius: @border-radius-lg @border-radius-lg 0 0;
    padding: 0.5rem 1rem 0;
    gap: 0.5rem;
    border-bottom: 1px solid var(--ui-border);
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
      color: var(--ui-text-secondary);
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
        background: var(--ui-accent);
        border-radius: 3px 3px 0 0;
        .transition(transform);
      }

      &:hover {
        color: var(--ui-text-primary);
        background: var(--ui-accent-soft);
      }

      &.active {
        color: var(--ui-accent);
        background: var(--ui-surface);
        box-shadow: var(--ui-shadow-sm);
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
      border: 1px solid var(--ui-border);
      box-shadow: var(--ui-shadow-md);
      background: var(--ui-surface-strong);
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
          color: var(--ui-text-secondary);
        }

        &:hover {
          background: var(--ui-accent-soft);
        }
        &.active {
          background: rgba(var(--ui-accent-rgb), 0.2);
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

.risk-confirm-intro {
  margin: 0 0 1rem;
  color: var(--ui-text-secondary);
}

.risk-confirm-list {
  display: grid;
  gap: 0.75rem;
}

.risk-item {
  padding: 1rem;
  border: 1px solid var(--ui-border);
  border-radius: @border-radius-md;
  background: var(--ui-surface);

  &.critical {
    border-color: var(--ui-danger-border);
    background: var(--ui-danger-soft);
  }

  &.high {
    border-color: color-mix(in srgb, var(--ui-warning) 36%, transparent);
    background: color-mix(in srgb, var(--ui-warning) 8%, transparent);
  }

  &.medium {
    border-color: color-mix(in srgb, var(--ui-accent) 30%, transparent);
    background: var(--ui-accent-soft);
  }

  p {
    margin: 0.5rem 0 0;
    color: var(--ui-text-primary);
    line-height: 1.5;
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
    color: var(--ui-danger-contrast);
    background: var(--ui-danger);
  }

  &.high {
    color: var(--ui-warning-contrast);
    background: var(--ui-warning);
  }

  &.medium {
    color: var(--ui-accent-contrast);
    background: var(--ui-accent);
  }
}

.risk-detail,
.risk-recovery {
  margin-top: 0.7rem;
  padding-top: 0.7rem;
  border-top: 1px solid var(--ui-border);

  span {
    display: block;
    margin-bottom: 0.25rem;
    color: var(--ui-text-secondary);
    font-size: 0.8rem;
    font-weight: 600;
  }

  code {
    display: inline-block;
    max-width: 100%;
    overflow-wrap: anywhere;
    padding: 0.2rem 0.4rem;
    border-radius: @border-radius-sm;
    color: var(--ui-text-primary);
    background: var(--ui-accent-soft);
  }

}

.risk-recovery p {
  margin: 0;
  color: var(--ui-text-secondary);
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
    border-radius: var(--ui-radius-md);
    border: 1px solid var(--ui-border-strong);
    color: var(--ui-accent-contrast);
    font-size: 1.25rem;
    cursor: pointer;
    position: relative;
    .transition();
    .flex-center();

    &:hover {
      transform: translateY(-1px);
      box-shadow: var(--ui-shadow-md);
    }

    &:active {
      transform: scale(0.97) translateY(0);
      transition: transform 0.1s @cubic-bounce;
    }

    &:focus-visible {
      outline: none;
      box-shadow: var(--ui-shadow-sm), 0 0 0 3px var(--ui-accent-soft);
    }

    &-primary {
      background: var(--ui-accent);
      color: var(--ui-accent-contrast);
      box-shadow: var(--ui-shadow-sm);

      &:hover {
        background: var(--ui-accent);
        box-shadow: var(--ui-shadow-md);
      }

      &.has-unsaved {
        animation: pulse-warning 2s ease-in-out 3;
        box-shadow: var(--ui-shadow-sm), 0 0 0 3px color-mix(in srgb, var(--ui-warning) 50%, transparent);

        &:hover {
          animation: pulse-warning 2s ease-in-out 3;
          box-shadow: var(--ui-shadow-md), 0 0 0 4px color-mix(in srgb, var(--ui-warning) 70%, transparent);
        }
      }
    }

    &-success {
      background: var(--ui-success);
      color: var(--ui-success-contrast);
      box-shadow: var(--ui-shadow-sm);

      &:hover {
        background: var(--ui-success);
        box-shadow: var(--ui-shadow-md);
      }
    }

    i {
      position: relative;
      z-index: 1;
      .transition(transform);
    }

    &:hover i {
      transform: scale(1.06);
    }

    &:disabled {
      cursor: not-allowed;
      opacity: 0.65;
      transform: none;
      box-shadow: none;
      animation: none;
      border-color: var(--ui-border);
      background: var(--ui-surface-strong);
      color: var(--ui-text-muted);

      i {
        transform: none;
      }
    }
  }

  @keyframes pulse-warning {
    0%,
    100% {
      box-shadow: var(--ui-shadow-sm), 0 0 0 3px color-mix(in srgb, var(--ui-warning) 50%, transparent);
    }
    50% {
      box-shadow: var(--ui-shadow-sm), 0 0 0 5px color-mix(in srgb, var(--ui-warning) 80%, transparent);
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
    gap: 0.35rem;
    overflow-x: auto;
    overflow-y: hidden;
    flex-wrap: nowrap;
    scroll-padding-inline: 2rem;
    scroll-snap-type: x proximity;
    scrollbar-width: thin;
    -webkit-overflow-scrolling: touch;

    .nav-link {
      min-height: 44px;
      padding: 0.625rem 1rem;
      font-size: 0.875rem;
      white-space: nowrap;
      scroll-snap-align: center;
    }
  }

  .page-config .config-tabs-shell {
    &::before,
    &::after {
      content: '';
      position: absolute;
      top: 0;
      bottom: 0;
      width: 2rem;
      pointer-events: none;
      z-index: 11;
    }

    &::before {
      left: 0;
      background: linear-gradient(90deg, var(--ui-surface-strong), transparent);
    }

    &::after {
      right: 0;
      background: linear-gradient(270deg, var(--ui-surface-strong), transparent);
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
