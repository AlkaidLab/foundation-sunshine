<template>
  <div>
    <Navbar v-if="!showSetupWizard" />

    <!-- 首次设置向导 -->
    <SetupWizard
      v-if="showSetupWizard"
      :adapters="adapters"
      :display-devices="displayDevices"
      :has-locale="hasLocale"
      @setup-complete="onSetupComplete"
    />

    <!-- 正常首页内容 -->
    <div v-if="!showSetupWizard" id="content" class="container">
      <div class="page-header mb-4">
        <h1 class="page-title">
          {{ $t('index.welcome') }}
        </h1>
      </div>
      <p>{{ $t('index.description') }}</p>

      <!-- 错误日志 -->
      <ErrorLogs :fatal-logs="fatalLogs" />

      <!-- 版本信息 -->
      <VersionCard
        :version="version"
        :github-version="githubVersion"
        :pre-release-version="preReleaseVersion"
        :notify-pre-releases="notifyPreReleases"
        :loading="loading"
        :installed-version-not-stable="installedVersionNotStable"
        :stable-build-available="stableBuildAvailable"
        :pre-release-build-available="preReleaseBuildAvailable"
        :build-version-is-dirty="buildVersionIsDirty"
        :parsed-stable-body="parsedStableBody"
        :parsed-pre-release-body="parsedPreReleaseBody"
      />

      <!-- 资源卡片 -->
      <div class="my-4">
        <ResourceCard />
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import SetupWizard from '../components/SetupWizard.vue'
import ResourceCard from '../components/common/ResourceCard.vue'
import ErrorLogs from '../components/common/ErrorLogs.vue'
import VersionCard from '../components/common/VersionCard.vue'
import { useVersion } from '../composables/useVersion.js'
import { useLogs } from '../composables/useLogs.js'
import { useSetupWizard } from '../composables/useSetupWizard.js'
import { trackEvents } from '../config/firebase.js'

// 使用组合式函数
const {
  version,
  githubVersion,
  preReleaseVersion,
  notifyPreReleases,
  loading,
  installedVersionNotStable,
  stableBuildAvailable,
  preReleaseBuildAvailable,
  buildVersionIsDirty,
  parsedStableBody,
  parsedPreReleaseBody,
  fetchVersions,
} = useVersion()

const { fatalLogs, fetchLogs } = useLogs()

const { showSetupWizard, adapters, displayDevices, hasLocale, checkSetupWizard, onSetupComplete } = useSetupWizard()

// 上报显卡信息
const reportGPUInfo = (config) => {
  try {
    const adapters = config.adapters || []
    const adapterNames = adapters.map((a) => (typeof a === 'string' ? a : a?.name || String(a))).join(', ')

    const gpuInfo = {
      platform: config.platform || 'unknown',
      adapter_count: adapters.length,
      adapters: adapterNames,
      selected_adapter: config.adapter_name || (adapters.length ? 'auto' : 'none'),
      has_selected_adapter: !!config.adapter_name,
    }

    trackEvents.gpuReported(gpuInfo)
  } catch (error) {
    console.error('上报显卡信息失败:', error)
  }
}

// 初始化
onMounted(async () => {
  // 记录页面访问
  trackEvents.pageView('home')

  try {
    const config = await fetch('/api/config').then((r) => r.json())

    setTimeout(() => {
      reportGPUInfo(config)
    }, 1000)

    // 检查是否需要显示设置向导
    if (checkSetupWizard(config)) {
      return
    }

    // 获取版本信息
    await fetchVersions(config)

    // 获取日志
    await fetchLogs()

    // 更新页面标题
    if (version.value) {
      document.title += ` Ver ${version.value.version}`
    }
  } catch (e) {
    // 在预览模式下，API 不可用是正常的，只记录警告
    if (e?.message?.includes('JSON') || e?.message?.includes('<!DOCTYPE')) {
      console.warn('API not available in preview mode:', e.message)
    } else {
      console.error('Failed to initialize:', e)
      trackEvents.errorOccurred('home_initialization', e.message)
    }
  }
})
</script>

<style scoped>
.container {
  padding: 1rem;
}

.page-title {
  text-shadow: 2px 2px 6px rgba(0, 0, 0, 0.15), 0 0 10px rgba(0, 0, 0, 0.1);
}

[data-bs-theme='dark'] .page-title {
  text-shadow: 2px 2px 8px rgba(0, 0, 0, 0.5), 0 0 12px rgba(0, 0, 0, 0.3);
}
</style>
