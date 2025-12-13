<template>
  <div class="page-config">
    <Navbar />
    <div class="config-floating-buttons">
      <button class="cute-btn cute-btn-primary" @click="save" :title="$t('_common.save')">
        <i class="fas fa-save"></i>
      </button>
      <button class="cute-btn cute-btn-success" @click="apply" v-if="saved && !restarted" :title="$t('_common.apply')">
        <i class="fas fa-check"></i>
      </button>
      <!-- Toast notifications for save/apply success -->
      <div class="floating-toast-container">
        <div
          class="toast align-items-center text-bg-success border-0"
          :class="{ show: showSaveToast }"
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
        <div
          class="toast align-items-center text-bg-success border-0 mt-2"
          :class="{ show: showRestartToast }"
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
      </div>
    </div>
    <div class="container">
      <h1 class="my-4 page-title">{{ $t('config.configuration') }}</h1>
      
      <div class="form card config-skeleton" v-if="!config">
        <div class="card-header skeleton-header">
          <div class="skeleton-tabs">
            <div class="skeleton-tab" v-for="n in 6" :key="n"></div>
          </div>
        </div>
        <div class="config-page skeleton-body">
          <div class="skeleton-section">
            <div class="skeleton-title"></div>
            <div class="skeleton-row" v-for="n in 4" :key="n">
              <div class="skeleton-label"></div>
              <div class="skeleton-input"></div>
            </div>
          </div>
          <div class="skeleton-section">
            <div class="skeleton-title"></div>
            <div class="skeleton-row" v-for="n in 3" :key="n">
              <div class="skeleton-label"></div>
              <div class="skeleton-input"></div>
            </div>
          </div>
        </div>
      </div>
      
      <div class="form card" v-if="config">
        <!-- Header -->
        <ul class="nav nav-tabs config-tabs card-header">
          <template v-for="tab in tabs" :key="tab.id">
            <!-- 分组标签页（编码器） -->
            <li
              v-if="tab.type === 'group' && tab.children"
              class="nav-item dropdown"
              :class="{ active: isEncoderTabActive(tab), show: expandedDropdown === tab.id }"
            >
              <a
                class="nav-link dropdown-toggle"
                :class="{ active: isEncoderTabActive(tab) }"
                href="#"
                @click.prevent="toggleEncoderDropdown(tab.id, $event)"
                :id="`dropdown-${tab.id}`"
                role="button"
                :aria-expanded="expandedDropdown === tab.id"
              >
                {{ $t(`tabs.${tab.id}`) || tab.name }}
              </a>
              <ul
                class="dropdown-menu"
                :class="{ show: expandedDropdown === tab.id }"
                :aria-labelledby="`dropdown-${tab.id}`"
              >
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
            <!-- 普通标签页 -->
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

        <!-- General Tab -->
        <General
          v-if="currentTab === 'general'"
          :config="config"
          :global-prep-cmd="global_prep_cmd"
          :platform="platform"
        />

        <!-- Input Tab -->
        <Inputs v-if="currentTab === 'input'" :config="config" :platform="platform" />

        <!-- Audio/Video Tab -->
        <AudioVideo
          v-if="currentTab === 'av'"
          :config="config"
          :platform="platform"
          :resolutions="resolutions"
          :fps="fps"
          :display-mode-remapping="display_mode_remapping"
        />

        <!-- Network Tab -->
        <Network v-if="currentTab === 'network'" :config="config" :platform="platform" />

        <!-- Files Tab -->
        <Files v-if="currentTab === 'files'" :config="config" :platform="platform" />

        <!-- Advanced Tab -->
        <Advanced v-if="currentTab === 'advanced'" :config="config" :platform="platform" />

        <ContainerEncoders :current-tab="currentTab" :config="config" :platform="platform" />
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, watch, onMounted, provide, computed, onUnmounted } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import General from '../configs/tabs/General.vue'
import Inputs from '../configs/tabs/Inputs.vue'
import Network from '../configs/tabs/Network.vue'
import Files from '../configs/tabs/Files.vue'
import Advanced from '../configs/tabs/Advanced.vue'
import AudioVideo from '../configs/tabs/AudioVideo.vue'
import ContainerEncoders from '../configs/tabs/ContainerEncoders.vue'
import { useConfig } from '../composables/useConfig.js'
import { initFirebase, trackEvents } from '../config/firebase.js'

// 初始化Firebase Analytics
initFirebase()

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
  save,
  apply,
  handleHash,
} = useConfig()

// Toast 状态
const showSaveToast = ref(false)
const showRestartToast = ref(false)

// 下拉菜单展开状态
const expandedDropdown = ref(null)

// 检查编码器分组是否处于激活状态
const isEncoderTabActive = (tab) => {
  return tab.type === 'group' && tab.children?.some((child) => child.id === currentTab.value)
}

// 切换编码器下拉菜单
const toggleEncoderDropdown = (tabId, event) => {
  event.stopPropagation()
  
  if (expandedDropdown.value === tabId) {
    expandedDropdown.value = null
    return
  }
  
  expandedDropdown.value = tabId
  
  // 如果当前没有选中任何编码器子标签，选中第一个可用的
  const encoderGroup = tabs.value.find((t) => t.id === tabId && t.type === 'group')
  const children = encoderGroup?.children
  
  if (children?.length && !children.some((child) => child.id === currentTab.value)) {
    currentTab.value = children[0].id
  }
}

// 选择编码器子标签
const selectEncoderTab = (childTabId, event) => {
  event.stopPropagation()
  currentTab.value = childTabId
  expandedDropdown.value = null
}

// Toast 显示逻辑
const showToast = (toastRef, duration = 5000) => {
  toastRef.value = true
  setTimeout(() => {
    toastRef.value = false
  }, duration)
}

// 监听 saved 和 restarted 状态变化来显示 toast
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

// 提供平台信息给子组件
provide('platform', computed(() => platform.value))

// 点击外部关闭下拉菜单
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
})
</script>

<style lang="less">
@import '../styles/global.less';

// Variables
@transition-fast: 0.3s;
@transition-medium: 0.6s;
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
@color-apple-light: #333;
@color-apple-dark: #ccc;

// Mixins
.flex-center() {
  display: flex;
  justify-content: center;
  align-items: center;
}

.transition(@properties: all) {
  transition: @properties @transition-fast @cubic-smooth;
}

.config-page {
  padding: 1em;
  border: 1px solid transparent;
  border-top: none;
  backdrop-filter: blur(2px);
  transform: translateZ(0);
  backface-visibility: hidden;
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
    background: linear-gradient(90deg, rgba(0, 0, 0, 0.08) 25%, rgba(0, 0, 0, 0.12) 50%, rgba(0, 0, 0, 0.08) 75%);
    background-size: 200% 100%;
    animation: skeleton-shimmer 1.5s infinite;
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
    background: linear-gradient(90deg, rgba(0, 0, 0, 0.08) 25%, rgba(0, 0, 0, 0.12) 50%, rgba(0, 0, 0, 0.08) 75%);
    background-size: 200% 100%;
    animation: skeleton-shimmer 1.5s infinite;
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
    background: linear-gradient(90deg, rgba(0, 0, 0, 0.06) 25%, rgba(0, 0, 0, 0.1) 50%, rgba(0, 0, 0, 0.06) 75%);
    background-size: 200% 100%;
    animation: skeleton-shimmer 1.5s infinite;
    border-radius: 4px;
    flex-shrink: 0;
  }

  .skeleton-input {
    flex: 1;
    height: 38px;
    background: linear-gradient(90deg, rgba(0, 0, 0, 0.06) 25%, rgba(0, 0, 0, 0.1) 50%, rgba(0, 0, 0, 0.06) 75%);
    background-size: 200% 100%;
    animation: skeleton-shimmer 1.5s infinite;
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
  .skeleton-title {
    background: linear-gradient(90deg, rgba(255, 255, 255, 0.08) 25%, rgba(255, 255, 255, 0.12) 50%, rgba(255, 255, 255, 0.08) 75%);
    background-size: 200% 100%;
    animation: skeleton-shimmer 1.5s infinite;
  }

  .skeleton-label,
  .skeleton-input {
    background: linear-gradient(90deg, rgba(255, 255, 255, 0.06) 25%, rgba(255, 255, 255, 0.1) 50%, rgba(255, 255, 255, 0.06) 75%);
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

        // Encoder-specific first letter colors
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

.toast {
  opacity: 0;
  transition: opacity @transition-fast ease-in-out;

  &.show { opacity: 1; }
}

.config-floating-buttons {
  position: sticky;
  top: 2rem;
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

    .toast { margin-bottom: 0.5rem; }
  }

  .cute-btn {
    width: @btn-size;
    height: @btn-size;
    border-radius: 50%;
    border: 2px solid rgba(255, 255, 255, 0.3);
    color: #fff;
    font-size: 1.25rem;
    cursor: pointer;
    backdrop-filter: blur(10px);
    box-shadow: 0 4px 15px rgba(0, 0, 0, 0.2), 0 2px 8px rgba(0, 0, 0, 0.15);
    position: relative;
    overflow: hidden;
    transition: all @transition-fast @cubic-bounce;
    .flex-center();

    &::before {
      content: '';
      position: absolute;
      inset: 0;
      background: rgba(255, 255, 255, 0.3);
      border-radius: 50%;
      transform: scale(0);
      transition: transform @transition-medium ease;
    }

    i {
      position: relative;
      z-index: 2;
      transition: transform @transition-fast ease;
    }

    &-primary {
      background: linear-gradient(135deg, #667eea, #764ba2);
      &:hover { background: linear-gradient(135deg, #764ba2, #667eea); }
    }

    &-success {
      background: linear-gradient(135deg, #11998e, #38ef7d);
      &:hover { background: linear-gradient(135deg, #38ef7d, #11998e); }
    }
  }
}

// Dark mode
[data-bs-theme='dark'] .page-config .config-tabs {
  background: linear-gradient(135deg, rgba(255, 255, 255, 0.1), rgba(255, 255, 255, 0.05));
  border-bottom-color: rgba(255, 255, 255, 0.1);

  .nav-link {
    &:hover { background: rgba(var(--bs-primary-rgb), 0.15); }
    &.active { box-shadow: 0 -4px 12px rgba(0, 0, 0, 0.3); }
  }

  .dropdown-menu {
    border-color: rgba(255, 255, 255, 0.1);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);

    .dropdown-item {
      &:hover { background: rgba(var(--bs-primary-rgb), 0.15); }
      &.active { background: rgba(var(--bs-primary-rgb), 0.25); }
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
</style>
