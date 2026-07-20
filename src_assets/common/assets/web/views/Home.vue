<template>
  <div class="home-page">
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
    <main v-if="!showSetupWizard" id="content" class="container home-content">
      <div class="page-header home-intro">
        <div>
          <p class="home-eyebrow">
            <i class="fas fa-satellite-dish me-2" aria-hidden="true"></i>
            Sunshine WebUI
          </p>
          <h1 class="page-title">{{ $t('index.welcome') }}</h1>
          <p class="page-subtitle">{{ $t('index.description') }}</p>
        </div>
        <div class="home-status" :class="`home-status-${hostStatus}`" role="status" :aria-live="hostStatus === 'loading' ? 'polite' : 'off'">
          <span class="home-status-dot" aria-hidden="true"></span>
          <span>{{ statusLabel }}</span>
        </div>
      </div>

      <section class="host-overview card shadow-sm" aria-labelledby="host-overview-title">
        <div class="host-overview-main">
          <div class="section-kicker" id="host-overview-title">
            <i class="fas fa-server me-2" aria-hidden="true"></i>
            {{ $t('navbar.home') }}
          </div>
          <h2 class="host-name">{{ hostConfig?.sunshine_name || 'Sunshine' }}</h2>
          <p class="host-meta">
            <span v-if="hostConfig?.platform">{{ hostConfig.platform }}</span>
            <span v-if="hostConfig?.platform" aria-hidden="true"> · </span>
            <span>{{ versionLabel }}</span>
          </p>
        </div>

        <div class="host-metrics" aria-label="Host summary">
          <div class="host-metric">
            <span class="metric-value">{{ clientsCount ?? '—' }}</span>
            <span class="metric-label">{{ $t('navbar.pin') }}</span>
          </div>
          <div class="host-metric">
            <span class="metric-value">{{ appsCount ?? '—' }}</span>
            <span class="metric-label">{{ $t('navbar.applications') }}</span>
          </div>
        </div>

        <nav class="quick-actions" :aria-label="$t('resource_card.quick_start')">
          <a
            v-for="action in quickActions"
            :key="action.id"
            class="quick-action"
            :class="{ 'quick-action-primary': action.primary }"
            :href="action.href"
          >
            <i :class="action.icon" aria-hidden="true"></i>
            <span>{{ $t(action.labelKey) }}</span>
            <small>{{ $t(action.descriptionKey) }}</small>
          </a>
        </nav>
      </section>

      <div v-if="hostStatus === 'error'" class="home-alert" role="alert">
        <i class="fas fa-triangle-exclamation" aria-hidden="true"></i>
        <div>
          <strong>{{ $t('_common.error') }}</strong>
          <p>{{ $t('index.startup_errors').replace(/<[^>]+>/g, '') }}</p>
        </div>
      </div>

      <!-- 错误日志 -->
      <ErrorLogs :fatal-logs="fatalLogs" />

      <!-- 版本信息 -->
      <VersionCard
        v-if="showVersionDetails"
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
      <ResourceCard class="home-resources" />
    </main>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import Navbar from '../components/layout/Navbar.vue'
import SetupWizard from '../components/SetupWizard.vue'
import ResourceCard from '../components/common/ResourceCard.vue'
import ErrorLogs from '../components/common/ErrorLogs.vue'
import VersionCard from '../components/common/VersionCard.vue'
import { useVersion } from '../composables/useVersion.js'
import { useLogs } from '../composables/useLogs.js'
import { useSetupWizard } from '../composables/useSetupWizard.js'
import { trackEvents } from '../config/firebase.js'
import { apiJson } from '../utils/apiFetch.js'

const { t } = useI18n()
const hostConfig = ref(null)
const hostStatus = ref('loading')
const appsCount = ref(null)
const clientsCount = ref(null)

const quickActions = [
  {
    id: 'pin',
    href: '/pin',
    icon: 'fas fa-link',
    labelKey: 'navbar.pin',
    descriptionKey: 'pin.pin_pairing',
    primary: true,
  },
  {
    id: 'apps',
    href: '/apps',
    icon: 'fas fa-gamepad',
    labelKey: 'navbar.applications',
    descriptionKey: 'apps.applications_title',
  },
  {
    id: 'config',
    href: '/config',
    icon: 'fas fa-sliders-h',
    labelKey: 'navbar.configuration',
    descriptionKey: 'config.configuration',
  },
]

const statusLabel = computed(() => {
  if (hostStatus.value === 'loading') return t('index.loading_latest')
  if (hostStatus.value === 'error') return t('_common.error')
  return t('index.version_latest')
})

const versionLabel = computed(() => {
  const currentVersion = version.value?.version || hostConfig.value?.version
  return currentVersion ? `Ver ${currentVersion}` : t('index.loading_latest')
})

const showVersionDetails = computed(
  () =>
    buildVersionIsDirty.value ||
    installedVersionNotStable.value ||
    stableBuildAvailable.value ||
    (notifyPreReleases.value && preReleaseBuildAvailable.value),
)

const countCollection = (payload, key) => {
  if (Array.isArray(payload)) return payload.length
  if (Array.isArray(payload?.[key])) return payload[key].length
  if (key === 'clients' && Array.isArray(payload?.named_certs)) return payload.named_certs.length
  return null
}

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
    const config = await apiJson('/api/config')
    hostConfig.value = config
    hostStatus.value = 'ready'

    setTimeout(() => {
      reportGPUInfo(config)
    }, 1000)

    // 检查是否需要显示设置向导
    if (checkSetupWizard(config)) {
      return
    }

    const [appsResult, clientsResult] = await Promise.allSettled([
      apiJson('/api/apps'),
      apiJson('/api/clients/list'),
    ])
    if (appsResult.status === 'fulfilled') appsCount.value = countCollection(appsResult.value, 'apps')
    if (clientsResult.status === 'fulfilled') clientsCount.value = countCollection(clientsResult.value, 'clients')

    // 版本和日志互不阻塞，避免外部版本检查拖延本机状态提示
    await Promise.allSettled([fetchVersions(config), fetchLogs()])

    // 更新页面标题
    if (version.value) {
      document.title += ` Ver ${version.value.version}`
    }
  } catch (e) {
    hostStatus.value = 'error'
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

<style>
@import '../styles/global.less';

.home-content {
  max-width: 1180px;
  padding-top: 0.5rem;
}

.home-intro {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 1.5rem;
  margin: 0.5rem 0 0.75rem;
}

.home-eyebrow,
.section-kicker {
  margin: 0 0 0.2rem;
  color: var(--ui-accent, #4a9eff);
  font-size: 0.75rem;
  font-weight: 700;
  letter-spacing: 0.12em;
  text-transform: uppercase;
}

.home-intro .page-title {
  margin-bottom: 0.25rem;
  font-size: 1.65rem;
  line-height: 1.15;
  font-weight: 600;
  letter-spacing: -0.03em;
}

.home-intro .page-subtitle {
  max-width: 48rem;
}

.home-status {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  flex-shrink: 0;
  padding: 0.55rem 0.85rem;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: 999px;
  background: var(--ui-surface, rgba(255, 255, 255, 0.62));
  box-shadow: var(--ui-shadow-sm, 0 4px 16px rgba(74, 158, 255, 0.1));
  color: var(--ui-text-primary, #1e293b);
  backdrop-filter: blur(12px);
  font-size: 0.8rem;
  font-weight: 600;
}

.home-status-dot {
  width: 0.55rem;
  height: 0.55rem;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 0 0.2rem var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
}

.home-status-ready {
  color: var(--ui-success-text, #287d4c);
}

.home-status-loading {
  color: var(--ui-warning-text, #9a6700);
}

.home-status-error {
  color: var(--ui-danger-text, #b4233a);
}

.host-overview {
  display: grid;
  grid-template-columns: minmax(12rem, 1fr) auto minmax(29rem, 2.2fr);
  align-items: center;
  gap: 1rem 1.5rem;
  padding: 0.9rem 1.15rem;
  margin-bottom: 0.75rem;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: var(--ui-radius-md, 12px);
  background: var(--ui-surface-strong, rgba(240, 248, 255, 0.88));
  box-shadow: var(--ui-shadow-md, 0 12px 32px rgba(58, 126, 213, 0.14));
  backdrop-filter: blur(16px);
}

.host-overview-main {
  min-width: 0;
}

.host-name {
  margin: 0;
  overflow: hidden;
  color: var(--ui-text-primary, #1e293b);
  font-size: clamp(1.25rem, 2.5vw, 1.75rem);
  font-weight: 650;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.host-meta {
  margin: 0.35rem 0 0;
  color: var(--ui-text-secondary, #64748b);
  font-size: 0.85rem;
}

.host-metrics {
  display: flex;
  align-items: stretch;
  gap: 1rem;
}

.host-metric {
  display: flex;
  min-width: 4.25rem;
  flex-direction: column;
  justify-content: center;
  padding-left: 1rem;
  border-left: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
}

.metric-value {
  color: var(--ui-text-primary, #1e293b);
  font-size: 1.5rem;
  font-weight: 700;
  line-height: 1;
}

.metric-label {
  margin-top: 0.35rem;
  color: var(--ui-text-secondary, #64748b);
  font-size: 0.75rem;
}

.quick-actions {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 0.5rem;
}

.quick-action {
  display: grid;
  grid-template-columns: 1.5rem minmax(0, 1fr);
  column-gap: 0.5rem;
  align-items: center;
  padding: 0.65rem 0.75rem;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: var(--ui-radius-md, 12px);
  background: var(--ui-surface, rgba(255, 255, 255, 0.62));
  color: var(--ui-text-primary, #1e293b);
  text-decoration: none;
  transition: transform 0.2s ease, background 0.2s ease, border-color 0.2s ease;
}

.quick-action:hover,
.quick-action:focus-visible {
  border-color: var(--ui-border-strong, rgba(74, 158, 255, 0.42));
  background: var(--ui-surface-hover, rgba(255, 255, 255, 0.8));
  color: var(--ui-text-primary, #1e293b);
  transform: translateY(-2px);
}

.quick-action-primary {
  border-color: var(--ui-border-strong, rgba(74, 158, 255, 0.42));
  background: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
}

.quick-action > i {
  grid-row: span 2;
  color: var(--ui-accent, #4a9eff);
  font-size: 1rem;
  text-align: center;
}

.quick-action span {
  font-weight: 650;
}

.quick-action small {
  color: var(--ui-text-secondary, #64748b);
  font-size: 0.75rem;
}

.home-resources {
  margin-bottom: 0.25rem;
}

@media (max-width: 1199.98px) {
  .host-overview {
    grid-template-columns: minmax(0, 1fr) auto;
  }

  .quick-actions {
    grid-column: 1 / -1;
  }
}

.home-alert {
  display: flex;
  align-items: flex-start;
  gap: 0.75rem;
  margin: 1rem 0;
  padding: 0.9rem 1rem;
  border: 1px solid var(--ui-danger-border, rgba(180, 35, 58, 0.28));
  border-radius: var(--ui-radius-md, 12px);
  background: var(--ui-danger-soft, rgba(180, 35, 58, 0.1));
  color: var(--ui-danger-text, #b4233a);
}

.home-alert > i {
  margin-top: 0.15rem;
  color: var(--ui-danger-text, #b4233a);
}

.home-alert p {
  margin: 0.25rem 0 0;
  font-size: 0.85rem;
}

@media (max-width: 767.98px) {
  .home-intro {
    align-items: flex-start;
    flex-direction: column;
    gap: 0.9rem;
  }

  .home-intro .page-title {
    font-size: 1.8rem;
  }

  .host-overview {
    grid-template-columns: 1fr;
    gap: 1rem;
    padding: 1rem;
  }

  .host-metrics {
    gap: 0;
  }

  .host-metric {
    flex: 1;
    min-width: 0;
    padding-left: 0;
  }

  .host-metric + .host-metric {
    padding-left: 1rem;
    border-left: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  }

  .quick-actions {
    grid-template-columns: 1fr;
  }
}
</style>
