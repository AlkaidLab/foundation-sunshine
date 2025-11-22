<template>
  <div>
    <Navbar />
    <div class="container">
      <h1 class="my-4">{{ $t('config.configuration') }}</h1>
      <div class="form card" v-if="config">
        <!-- Header -->
        <ul class="nav nav-tabs">
          <li class="nav-item" v-for="tab in tabs" :key="tab.id">
            <a 
              class="nav-link" 
              :class="{'active': tab.id === currentTab}" 
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
        <Inputs 
          v-if="currentTab === 'input'" 
          :config="config" 
          :platform="platform" 
        />

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
        <Network 
          v-if="currentTab === 'network'" 
          :config="config" 
          :platform="platform" 
        />

        <!-- Files Tab -->
        <Files 
          v-if="currentTab === 'files'" 
          :config="config" 
          :platform="platform" 
        />

        <!-- Advanced Tab -->
        <Advanced 
          v-if="currentTab === 'advanced'" 
          :config="config" 
          :platform="platform" 
        />

        <ContainerEncoders 
          :current-tab="currentTab" 
          :config="config" 
          :platform="platform" 
        />
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
provide('platform', computed(() => platform.value))

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

<style scoped>
.config-page {
  padding: 1em;
  border: 1px solid transparent;
  border-top: none;
}

.buttons {
  padding: 1em 0;
}

.ms-item {
  border: 1px solid;
  border-radius: 2px;
  font-size: 12px;
  font-weight: bold;
}
</style>

