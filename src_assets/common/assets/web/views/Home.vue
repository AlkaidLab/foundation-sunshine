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
      <section class="home-hero" aria-labelledby="home-title">
        <div class="page-header home-intro">
          <div class="home-intro-copy">
            <h1 id="home-title" class="page-title">{{ $t('index.welcome') }}</h1>
            <p class="page-subtitle">{{ $t('index.description') }}</p>
          </div>
          <component
            :is="showVersionDetails ? 'button' : 'div'"
            class="home-status"
            :class="[`home-status-${statusState}`, { 'home-status-action': showVersionDetails }]"
            :type="showVersionDetails ? 'button' : undefined"
            :role="showVersionDetails ? 'button' : 'status'"
            :aria-live="statusState === 'loading' ? 'polite' : 'off'"
            :aria-label="statusAriaLabel"
            :aria-controls="showVersionDetails ? 'version-details' : undefined"
            :title="statusTitle"
            @click="handleStatusClick"
          >
            <span class="home-status-dot" aria-hidden="true"></span>
            <span>{{ statusLabel }}</span>
          </component>
        </div>

        <div class="host-overview">
          <div class="host-overview-main">
            <span class="host-mark" aria-hidden="true">
              <i class="fas fa-server"></i>
            </span>
            <div class="host-summary">
              <h2 class="host-name">{{ hostConfig?.sunshine_name || 'Sunshine' }}</h2>
              <p class="host-meta">
                <span v-if="hostConfig?.platform">{{ hostConfig.platform }}</span>
                <span v-if="hostConfig?.platform" aria-hidden="true"> · </span>
                <span>{{ versionLabel }}</span>
              </p>
            </div>
          </div>

          <div class="host-metrics" aria-label="Host summary">
            <div class="host-metric">
              <span class="metric-icon" aria-hidden="true"><i class="fas fa-display"></i></span>
              <span class="metric-value">{{ clientsCount ?? '—' }}</span>
              <span class="metric-label">{{ $t('navbar.clients') }}</span>
            </div>
            <div class="host-metric">
              <span class="metric-icon" aria-hidden="true"><i class="fas fa-gamepad"></i></span>
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
              <span class="quick-action-icon" aria-hidden="true"><i :class="action.icon"></i></span>
              <span class="quick-action-copy">
                <span>{{ $t(action.labelKey) }}</span>
                <small>{{ $t(action.descriptionKey) }}</small>
              </span>
              <i class="fas fa-chevron-right quick-action-arrow" aria-hidden="true"></i>
            </a>
          </nav>
        </div>
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
        id="version-details"
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
import { VERSION_CHECK_STATUS, useVersion } from '../composables/useVersion.js'
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
    labelKey: 'navbar.clients',
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

const hasStableUpdate = computed(() => stableBuildAvailable.value)
const hasPreReleaseUpdate = computed(() => notifyPreReleases.value && preReleaseBuildAvailable.value)

const statusState = computed(() => {
  if (hostStatus.value === 'error' || versionCheckStatus.value === VERSION_CHECK_STATUS.ERROR) return 'error'
  if (
    hostStatus.value === 'loading' ||
    versionCheckStatus.value === VERSION_CHECK_STATUS.IDLE ||
    versionCheckStatus.value === VERSION_CHECK_STATUS.CHECKING
  ) {
    return 'loading'
  }
  if (hasStableUpdate.value || hasPreReleaseUpdate.value) return 'update'
  return 'ready'
})

const statusLabel = computed(() => {
  if (statusState.value === 'loading') return t('index.loading_latest')
  if (statusState.value === 'error') return t('_common.error')
  if (hasStableUpdate.value) return t('index.new_stable')
  if (hasPreReleaseUpdate.value) return t('index.new_pre_release')
  return t('index.version_latest')
})

const statusTitle = computed(() =>
  versionCheckStatus.value === VERSION_CHECK_STATUS.ERROR ? t('welcome.network_error') : undefined,
)

const statusAriaLabel = computed(() => statusTitle.value || statusLabel.value)

const versionLabel = computed(() => {
  const currentVersion = version.value?.version || hostConfig.value?.version
  return currentVersion ? `Ver ${currentVersion}` : t('index.loading_latest')
})

const showVersionDetails = computed(
  () =>
    buildVersionIsDirty.value ||
    installedVersionNotStable.value ||
    hasStableUpdate.value ||
    hasPreReleaseUpdate.value,
)

const handleStatusClick = () => {
  if (!showVersionDetails.value) return
  document.getElementById('version-details')?.scrollIntoView({ behavior: 'smooth', block: 'start' })
}

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
  versionCheckStatus,
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
  display: flex;
  min-height: calc(100vh - 77px);
  max-width: 1180px;
  box-sizing: border-box;
  flex-direction: column;
  padding-top: 0.45rem;
  padding-bottom: 0.35rem;
}

.home-hero {
  position: relative;
  margin: 0.45rem 0 0.7rem;
  overflow: hidden;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: calc(var(--ui-radius-lg, 16px) + 2px);
  background: linear-gradient(
    145deg,
    color-mix(in srgb, var(--ui-surface-strong) 94%, var(--ui-accent-soft)),
    color-mix(in srgb, var(--ui-surface) 92%, transparent)
  );
  box-shadow: var(--ui-shadow-md, 0 12px 32px rgba(58, 126, 213, 0.14));
  backdrop-filter: blur(18px);
}

.home-hero::before,
.home-hero::after {
  position: absolute;
  border-radius: 50%;
  background: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
  content: '';
  pointer-events: none;
}

.home-hero::before {
  top: -3.2rem;
  right: 11rem;
  width: 8rem;
  height: 8rem;
  opacity: 0.65;
}

.home-hero::after {
  top: 1.25rem;
  right: 8.5rem;
  width: 0.65rem;
  height: 0.65rem;
  box-shadow: 1.1rem -0.75rem 0 -0.12rem var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
}

.home-intro {
  position: relative;
  z-index: 1;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  margin: 0;
  padding: 0.72rem 1rem 0.65rem 1.1rem;
}

.home-intro .page-title {
  margin-bottom: 0.18rem;
  font-size: 1.55rem;
  line-height: 1.15;
  font-weight: 650;
  letter-spacing: -0.03em;
}

.home-intro .page-subtitle {
  max-width: 48rem;
  font-size: 0.86rem;
}

.home-status {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
  flex-shrink: 0;
  padding: 0.45rem 0.7rem;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: var(--ui-radius-md, 12px);
  background: color-mix(in srgb, var(--ui-surface) 88%, transparent);
  color: var(--ui-text-primary, #1e293b);
  backdrop-filter: blur(10px);
  font-size: 0.76rem;
  font-weight: 600;
  font-family: inherit;
  text-align: left;
  transition: transform 0.2s ease, box-shadow 0.2s ease, border-color 0.2s ease, background-color 0.2s ease;
}

.home-status-action {
  appearance: none;
  cursor: pointer;
}

.home-status-action:hover,
.home-status-action:focus-visible {
  border-color: var(--ui-border-strong, rgba(74, 158, 255, 0.42));
  box-shadow: var(--ui-shadow-sm, 0 4px 16px rgba(74, 158, 255, 0.1));
  transform: translateY(-1px);
}

.home-status-action:focus-visible {
  outline: 2px solid var(--ui-accent, #4a9eff);
  outline-offset: 2px;
}

.home-status-dot {
  width: 0.48rem;
  height: 0.48rem;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 0 0.2rem color-mix(in srgb, currentColor 16%, transparent);
}

.home-status-ready {
  border-color: var(--ui-success-border, rgba(78, 205, 196, 0.34));
  background: var(--ui-success-soft, rgba(78, 205, 196, 0.12));
  color: var(--ui-success-text, #287d4c);
}

.home-status-loading {
  border-color: var(--ui-warning-border, rgba(255, 193, 7, 0.38));
  background: var(--ui-warning-soft, rgba(255, 193, 7, 0.13));
  color: var(--ui-warning-text, #9a6700);
}

.home-status-error {
  border-color: var(--ui-danger-border, rgba(255, 107, 107, 0.34));
  background: var(--ui-danger-soft, rgba(255, 107, 107, 0.12));
  color: var(--ui-danger-text, #b4233a);
}

.home-status-update {
  border-color: var(--ui-info-border, rgba(122, 184, 255, 0.34));
  background: var(--ui-info-soft, rgba(122, 184, 255, 0.12));
  color: var(--ui-info-text, #306fae);
}

.host-overview {
  position: relative;
  z-index: 1;
  display: grid;
  grid-template-columns: minmax(13rem, 1.1fr) auto minmax(29rem, 2fr);
  align-items: center;
  gap: 0.75rem 1rem;
  padding: 0.68rem 0.78rem;
  border-top: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  background: color-mix(in srgb, var(--ui-surface) 76%, transparent);
}

.host-overview-main {
  display: flex;
  align-items: center;
  gap: 0.7rem;
  min-width: 0;
}

.host-mark {
  display: inline-flex;
  width: 2.55rem;
  height: 2.55rem;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--ui-border-strong, rgba(74, 158, 255, 0.42));
  border-radius: 0.9rem;
  background: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
  color: var(--ui-accent, #4a9eff);
  box-shadow: inset 0 1px 0 color-mix(in srgb, white 40%, transparent);
  font-size: 1rem;
}

.host-summary {
  min-width: 0;
}

.host-name {
  margin: 0;
  overflow: hidden;
  color: var(--ui-text-primary, #1e293b);
  font-size: clamp(1.2rem, 2.3vw, 1.55rem);
  font-weight: 650;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.host-meta {
  margin: 0.22rem 0 0;
  color: var(--ui-text-secondary, #64748b);
  font-size: 0.76rem;
}

.host-metrics {
  display: flex;
  align-items: stretch;
  gap: 0.38rem;
}

.host-metric {
  display: grid;
  min-width: 5.3rem;
  grid-template-columns: 1.6rem auto;
  grid-template-rows: auto auto;
  column-gap: 0.45rem;
  align-items: center;
  padding: 0.45rem 0.55rem;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: var(--ui-radius-md, 12px);
  background: color-mix(in srgb, var(--ui-surface-strong) 76%, transparent);
}

.metric-icon {
  display: inline-flex;
  width: 1.6rem;
  height: 1.6rem;
  grid-row: 1 / 3;
  align-items: center;
  justify-content: center;
  border-radius: 0.55rem;
  background: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
  color: var(--ui-accent, #4a9eff);
  font-size: 0.7rem;
}

.metric-value {
  color: var(--ui-text-primary, #1e293b);
  font-size: 1.05rem;
  font-weight: 700;
  line-height: 1;
}

.metric-label {
  color: var(--ui-text-secondary, #64748b);
  font-size: 0.64rem;
  line-height: 1.1;
}

.quick-actions {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 0.38rem;
}

.quick-action {
  display: grid;
  min-width: 0;
  grid-template-columns: 1.8rem minmax(0, 1fr) auto;
  column-gap: 0.45rem;
  align-items: center;
  padding: 0.42rem 0.5rem;
  border: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  border-radius: var(--ui-radius-md, 12px);
  background-color: color-mix(in srgb, var(--ui-surface-strong) 78%, transparent);
  color: var(--ui-text-primary, #1e293b);
  text-decoration: none;
  transition: transform 0.2s ease, background-color 0.2s ease, border-color 0.2s ease, box-shadow 0.2s ease;
}

.quick-action:hover,
.quick-action:focus-visible {
  border-color: var(--ui-border-strong, rgba(74, 158, 255, 0.42));
  background-color: var(--ui-surface-hover, rgba(255, 255, 255, 0.8));
  color: var(--ui-text-primary, #1e293b);
  box-shadow: var(--ui-shadow-sm, 0 4px 16px rgba(74, 158, 255, 0.1));
  transform: translateY(-1px);
}

.quick-action-primary {
  border-color: var(--ui-border-strong, rgba(74, 158, 255, 0.42));
  background-color: color-mix(in srgb, var(--ui-accent-soft) 68%, var(--ui-surface-strong));
}

.quick-action-icon {
  display: inline-flex;
  width: 1.8rem;
  height: 1.8rem;
  align-items: center;
  justify-content: center;
  border-radius: 0.62rem;
  background: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
  color: var(--ui-accent, #4a9eff);
  font-size: 0.75rem;
}

.quick-action-copy {
  display: block;
  min-width: 0;
}

.quick-action-copy > span,
.quick-action-copy > small {
  display: block;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.quick-action-copy > span {
  font-size: 0.76rem;
  font-weight: 650;
}

.quick-action-copy > small {
  color: var(--ui-text-secondary, #64748b);
  font-size: 0.62rem;
}

.quick-action-arrow {
  color: var(--ui-text-muted, #94a3b8);
  font-size: 0.58rem;
  transition: transform 0.2s ease;
}

.quick-action:hover .quick-action-arrow,
.quick-action:focus-visible .quick-action-arrow {
  transform: translateX(2px);
}

.home-resources {
  display: flex;
  flex: 1 0 auto;
  flex-direction: column;
  margin-bottom: 0;
}

#version-details {
  scroll-margin-top: 1rem;
}

@media (min-width: 1440px) {
  .home-content {
    width: calc(100% - 2rem);
    max-width: 1760px;
  }
}

@media (min-width: 1440px) and (min-height: 820px) {
  .home-content {
    padding-top: 0.75rem;
  }

  .home-hero {
    margin-bottom: 0.9rem;
  }

  .home-intro {
    gap: 1.5rem;
    padding: 1rem 1.25rem 0.9rem 1.35rem;
  }

  .home-intro .page-title {
    font-size: 1.85rem;
  }

  .home-intro .page-subtitle {
    font-size: 0.95rem;
  }

  .home-status {
    padding: 0.55rem 0.9rem;
    font-size: 0.82rem;
  }

  .host-overview {
    grid-template-columns: minmax(16rem, 1.2fr) auto minmax(38rem, 2.3fr);
    gap: 1rem 1.25rem;
    padding: 0.9rem 1rem;
  }

  .host-mark {
    width: 3rem;
    height: 3rem;
    font-size: 1.1rem;
  }

  .host-name {
    font-size: 1.7rem;
  }

  .host-meta {
    font-size: 0.82rem;
  }

  .host-metrics,
  .quick-actions {
    gap: 0.55rem;
  }

  .host-metric {
    min-width: 6.25rem;
    grid-template-columns: 1.9rem auto;
    column-gap: 0.55rem;
    padding: 0.58rem 0.7rem;
  }

  .metric-icon {
    width: 1.9rem;
    height: 1.9rem;
    font-size: 0.78rem;
  }

  .metric-value {
    font-size: 1.2rem;
  }

  .metric-label {
    font-size: 0.7rem;
  }

  .quick-action {
    grid-template-columns: 2.1rem minmax(0, 1fr) auto;
    column-gap: 0.58rem;
    padding: 0.58rem 0.68rem;
  }

  .quick-action-icon {
    width: 2.1rem;
    height: 2.1rem;
    font-size: 0.82rem;
  }

  .quick-action-copy > span {
    font-size: 0.84rem;
  }

  .quick-action-copy > small {
    font-size: 0.68rem;
  }

  .home-resources .resource-groups {
    gap: 0.58rem;
  }

  .home-resources .resource-group {
    padding: 0.5rem 0.55rem 0.55rem;
  }

  .home-resources .resource-items {
    grid-auto-rows: 3.1rem;
    gap: 0.55rem;
  }

  .home-resources .resource-group-title {
    margin-bottom: 0.42rem;
    font-size: 0.78rem;
  }

  .home-resources .resource-link--compact {
    padding: 0.42rem 0.55rem;
  }

  .home-resources .resource-link--compact .resource-icon {
    width: 30px;
    height: 30px;
  }

  .home-resources .resource-link--compact .resource-title {
    font-size: 0.8rem;
  }

  .home-resources .resource-link--compact .resource-desc {
    font-size: 0.68rem;
  }

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
  .home-hero::before {
    right: -2.5rem;
  }

  .home-hero::after {
    display: none;
  }

  .home-intro {
    align-items: flex-start;
    flex-direction: column;
    gap: 0.65rem;
    padding: 0.85rem;
  }

  .home-intro .page-title {
    font-size: 1.55rem;
  }

  .home-status {
    align-self: stretch;
    justify-content: center;
  }

  .host-overview {
    grid-template-columns: 1fr;
    gap: 0.65rem;
    padding: 0.75rem;
  }

  .host-metrics {
    gap: 0.4rem;
  }

  .host-metric {
    flex: 1;
    min-width: 0;
    padding: 0.45rem 0.55rem;
  }

  .quick-actions {
    grid-template-columns: 1fr;
  }
}
</style>
