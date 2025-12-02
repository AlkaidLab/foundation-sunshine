<template>
  <div class="page-config">
    <Navbar />
    <div class="container">
      <h1 class="my-4 page-title">{{ $t('config.configuration') }}</h1>
      <div class="form card" v-if="config">
        <!-- Header -->
        <ul class="nav nav-tabs config-tabs">
          <li class="nav-item" v-for="tab in tabs" :key="tab.id">
            <a
              class="nav-link"
              :class="{ active: tab.id === currentTab }"
              href="#"
              @click.prevent="currentTab = tab.id"
            >
              {{ tab.name }}
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

      <!-- Save and Apply buttons -->
      <div class="alert alert-success my-4" v-if="saved && !restarted">
        <b>{{ $t('_common.success') }}</b> {{ $t('config.apply_note') }}
      </div>
      <div class="alert alert-success my-4" v-if="restarted">
        <b>{{ $t('_common.success') }}</b> {{ $t('config.restart_note') }}
      </div>
      <div class="mb-3 buttons">
        <button class="btn btn-primary" @click="save">{{ $t('_common.save') }}</button>
        <button class="btn btn-success" @click="apply" v-if="saved && !restarted">
          {{ $t('_common.apply') }}
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, provide, computed } from 'vue'
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

<style>
@import '../styles/global.css';
.config-page {
  padding: 1em;
  border: 1px solid transparent;
  border-top: none;
}

.page-config .form {
  backdrop-filter: blur(4px);
}

.page-config .nav-tabs {
  border: none;
}

.page-config .buttons {
  padding: 1em 0;
}

.page-config .ms-item {
  border: 1px solid;
  border-radius: 2px;
  font-size: 12px;
  font-weight: bold;
}

/* 美化配置页面标签栏 */
.page-config .config-tabs {
  background: linear-gradient(135deg, rgba(255, 255, 255, 0.1) 0%, rgba(255, 255, 255, 0.05) 100%);
  border-radius: 12px 12px 0 0;
  padding: 0.75rem 1rem 0;
  gap: 0.5rem;
  border-bottom: 1px solid rgba(0, 0, 0, 0.1);
}

.page-config .config-tabs .nav-item {
  margin-bottom: -1px;
}

.page-config .config-tabs .nav-link {
  border: none;
  border-radius: 10px 10px 0 0;
  padding: 0.75rem 1.5rem;
  font-weight: 500;
  color: var(--bs-secondary-color);
  background: transparent;
  transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  position: relative;
  overflow: hidden;
}

.page-config .config-tabs .nav-link::before {
  content: '';
  position: absolute;
  bottom: 0;
  left: 50%;
  transform: translateX(-50%) scaleX(0);
  width: 80%;
  height: 3px;
  background: linear-gradient(90deg, var(--bs-primary), var(--bs-info));
  border-radius: 3px 3px 0 0;
  transition: transform 0.3s cubic-bezier(0.4, 0, 0.2, 1);
}

.page-config .config-tabs .nav-link:hover {
  color: var(--bs-primary);
  background: rgba(var(--bs-primary-rgb), 0.08);
}

.page-config .config-tabs .nav-link.active {
  color: var(--bs-primary);
  background: var(--bs-body-bg);
  box-shadow: 0 -4px 12px rgba(0, 0, 0, 0.08);
  font-weight: 600;
}

.page-config .config-tabs .nav-link.active::before {
  transform: translateX(-50%) scaleX(1);
}

/* 暗色模式适配 */
[data-bs-theme='dark'] .page-config .config-tabs {
  background: linear-gradient(135deg, rgba(255, 255, 255, 0.05) 0%, rgba(255, 255, 255, 0.02) 100%);
  border-bottom-color: rgba(255, 255, 255, 0.1);
}

[data-bs-theme='dark'] .page-config .config-tabs .nav-link:hover {
  background: rgba(var(--bs-primary-rgb), 0.15);
}

[data-bs-theme='dark'] .page-config .config-tabs .nav-link.active {
  box-shadow: 0 -4px 12px rgba(0, 0, 0, 0.3);
}

/* 响应式优化 */
@media (max-width: 768px) {
  .page-config .config-tabs {
    padding: 0.5rem 0.5rem 0;
    gap: 0.25rem;
    overflow-x: auto;
    flex-wrap: nowrap;
  }

  .page-config .config-tabs .nav-link {
    padding: 0.5rem 1rem;
    font-size: 0.875rem;
    white-space: nowrap;
  }
}
</style>
