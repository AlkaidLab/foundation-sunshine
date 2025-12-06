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
      <div class="form card" v-if="config">
        <!-- Header -->
        <ul class="nav nav-tabs config-tabs card-header">
          <li class="nav-item" v-for="tab in tabs" :key="tab.id">
            <a
              class="nav-link"
              :class="{ active: tab.id === currentTab }"
              href="#"
              @click.prevent="currentTab = tab.id"
            >
              {{ $t(`tabs.${tab.id}`) || tab.name }}
            </a>
          </li>
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
import { ref, watch, onMounted, provide, computed } from 'vue'
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

// 监听 saved 和 restarted 状态变化来显示 toast
watch(saved, (newVal) => {
  if (newVal && !restarted.value) {
    showSaveToast.value = true
    setTimeout(() => {
      showSaveToast.value = false
    }, 5000)
  }
})

watch(restarted, (newVal) => {
  if (newVal) {
    showSaveToast.value = false
    showRestartToast.value = true
    setTimeout(() => {
      showRestartToast.value = false
    }, 5000)
  }
})

// 提供平台信息给子组件
provide(
  'platform',
  computed(() => platform.value)
)

onMounted(async () => {
  // 记录页面访问
  trackEvents.pageView('configuration')

  // 初始化标签页
  initTabs()

  // 加载配置
  await loadConfig()

  // 处理哈希导航
  handleHash()
  window.addEventListener('hashchange', handleHash)
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

.config-page {
  padding: 1em;
  border: 1px solid transparent;
  border-top: none;
  backdrop-filter: blur(2px);
  transform: translateZ(0);
  backface-visibility: hidden;
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

  // 配置页面标签栏
  .config-tabs {
    background: linear-gradient(135deg, rgba(255, 255, 255, 0.2), rgba(255, 255, 255, 0.1));
    border-radius: @border-radius-lg @border-radius-lg 0 0;
    padding: 0.75rem 1rem 0;
    gap: 0.5rem;
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);

    .nav-item {
      margin-bottom: -1px;
    }

    .nav-link {
      border: none;
      border-radius: @border-radius-md @border-radius-md 0 0;
      padding: 0.75rem 1.5rem;
      font-weight: 500;
      color: var(--bs-secondary-color);
      background: transparent;
      transition: all @transition-fast @cubic-smooth;
      position: relative;
      overflow: hidden;

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
        transition: transform @transition-fast @cubic-smooth;
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
    }
  }
}

// Toast 样式
.toast {
  opacity: 0;
  transition: opacity @transition-fast ease-in-out;

  &.show {
    opacity: 1;
  }
}

// 浮动按钮组
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

    .toast {
      margin-bottom: 0.5rem;
    }
  }

  .cute-btn {
    width: @btn-size;
    height: @btn-size;
    border-radius: 50%;
    border: 2px solid rgba(255, 255, 255, 0.3);
    color: #fff;
    font-size: 1.25rem;
    cursor: pointer;
    transition: all @transition-fast @cubic-bounce;
    backdrop-filter: blur(10px);
    box-shadow: 0 4px 15px rgba(0, 0, 0, 0.2), 0 2px 8px rgba(0, 0, 0, 0.15);
    display: flex;
    justify-content: center;
    align-items: center;
    position: relative;
    overflow: hidden;

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

      &:hover {
        background: linear-gradient(135deg, #764ba2, #667eea);
      }
    }

    &-success {
      background: linear-gradient(135deg, #11998e, #38ef7d);

      &:hover {
        background: linear-gradient(135deg, #38ef7d, #11998e);
      }
    }
  }
}

// 暗色模式适配
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
}

// 响应式优化
@media (max-width: 768px) {
  .config-floating-buttons {
    float: none;
    position: fixed;
    right: 1rem;
    bottom: 1rem;
    top: auto;
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
