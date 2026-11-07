<template>
  <div class="home-page">
    <Navbar v-if="!showSetupWizard" material-colors />
    <SetupWizard
      v-if="showSetupWizard"
      :adapters="adapters"
      :display-devices="displayDevices"
      :has-locale="hasLocale"
      @setup-complete="onSetupComplete"
    />
    <main v-else id="content" class="container home-content" :aria-busy="hostStatus === 'loading'">
      <header class="home-intro" style="--enter-delay: 0ms">
        <h1
          class="home-greeting"
          :class="{ 'home-greeting-cjk': greetingHasCjk, 'home-greeting-hang': greetingHangsPunct }"
        >
          <span class="home-greeting-whisper">{{ greetingText }}</span>
          <strong class="home-greeting-word"
            >Sunshine<span
              v-if="streamState === 'streaming'"
              class="home-greeting-eq"
              aria-hidden="true"
              ><i></i><i></i><i></i></span
            ><span v-else aria-hidden="true">!</span></strong
          >
        </h1>
        <p class="home-tagline home-enter" style="--enter-delay: 320ms">
          <span
            v-if="streamState === 'streaming' && streamArrival"
            :key="streamArrival.key"
            class="home-stream-arrival"
            >{{ copy.arrivalPrefix
            }}<span class="home-stream-target">{{ streamArrival.target }}</span
            >{{ copy.arrivalSuffix }}</span
          >
          <span v-else>{{ copy.tagline }}</span>
        </p>
      </header>
      <section
        class="host-overview home-enter"
        aria-labelledby="host-name"
        style="--enter-delay: 60ms"
      >
        <div class="host-summary">
          <h2 id="host-name" class="host-name" ref="hostNameEl">{{ hostConfig?.sunshine_name || 'Sunshine' }}</h2>
          <p class="host-platform">
            <span>{{ hostConfig?.platform || '—' }}</span>
            <span
              class="host-stream-status"
              :class="{ 'is-streaming': streamState === 'streaming' }"
              role="status"
              aria-live="polite"
              aria-atomic="true"
              >{{ copy.streamStatus[streamState] }}</span
            >
          </p>
        </div>
        <dl class="host-metrics">
          <div>
            <dt>{{ copy.clients }}</dt>
            <dd>{{ clientsCount ?? '—' }}</dd>
          </div>
          <div>
            <dt>{{ copy.apps }}</dt>
            <dd>{{ appsCount ?? '—' }}</dd>
          </div>
        </dl>
      </section>
      <nav
        class="home-primary-actions home-enter"
        :aria-label="copy.pair"
        style="--enter-delay: 120ms"
      >
        <a href="/pin" class="home-pair-action">{{ copy.pair }}</a>
        <a href="/pin" class="home-text-action"
          >{{ copy.manage
          }}<Icon
            name="chevron-right"
            :size="14"
            :stroke-width="1.8"
            icon-class="home-shortcut-arrow"
            aria-hidden="true"
            focusable="false"
        /></a>
      </nav>
      <div v-if="hostStatus === 'error'" class="home-alert" role="alert">
        <strong>{{ $t('_common.error') }}</strong>
        <p>{{ $t('index.startup_errors').replace(/<[^>]+>/g, '') }}</p>
      </div>
      <ErrorLogs :fatal-logs="fatalLogs" />
      <nav
        class="home-shortcuts home-enter"
        :aria-label="copy.applications"
        style="--enter-delay: 200ms"
      >
        <a href="/apps"
          ><h2>
            <span>{{ copy.applications }}</span
            ><Icon
              name="chevron-right"
              :size="16"
              :stroke-width="1.8"
              icon-class="home-shortcut-arrow"
              aria-hidden="true"
              focusable="false"
            />
          </h2>
          <p>{{ copy.applicationsHint }}</p></a
        >
        <a href="/config"
          ><h2>
            <span>{{ copy.settings }}</span
            ><Icon
              name="chevron-right"
              :size="16"
              :stroke-width="1.8"
              icon-class="home-shortcut-arrow"
              aria-hidden="true"
              focusable="false"
            />
          </h2>
          <p>{{ copy.settingsHint }}</p></a
        >
        <a href="/config#av" @click="openDisplay"
          ><h2>
            <span>{{ copy.display }}</span
            ><Icon
              name="chevron-right"
              :size="16"
              :stroke-width="1.8"
              icon-class="home-shortcut-arrow"
              aria-hidden="true"
              focusable="false"
            />
          </h2>
          <p>{{ copy.displayHint }}</p></a
        >
      </nav>
      <section
        class="home-recent home-enter"
        aria-labelledby="recent-connections-title"
        style="--enter-delay: 270ms"
      >
        <h2 id="recent-connections-title">{{ copy.recent }}</h2>
        <div class="recent-content" aria-live="polite">
          <p v-if="clientsLoading && clientsCount === null" class="home-secondary">
            {{ copy.loading }}
          </p>
          <div v-else-if="clientsError" class="recent-empty">
            <h3>{{ copy.unavailable }}</h3>
            <p>{{ copy.unavailableHint }}</p>
            <button type="button" class="home-text-action" @click="refreshClients">
              {{ copy.retry }}
            </button>
          </div>
          <ul v-else-if="recentConnections.length" class="recent-connections">
            <li v-for="client in recentConnections.slice(0, 3)" :key="client.uuid">
              <a href="/pin">
                <span class="recent-main">
                  <span class="recent-client-name">{{ client.name || copy.unnamed }}</span
                  ><span
                    v-if="streamingClients.has(client.uuid)"
                    class="recent-live"
                    role="img"
                    :aria-label="copy.streaming"
                  ><i></i><i></i><i></i></span>
                </span>
                <time
                  v-if="!streamingClients.has(client.uuid)"
                  class="recent-time"
                  :datetime="new Date(client.lastSeen).toISOString()"
                  >{{ formatLastSeen(client.lastSeen) }}</time
                >
                <Icon
                  name="chevron-right"
                  :size="14"
                  :stroke-width="1.8"
                  icon-class="recent-row-arrow"
                  aria-hidden="true"
                  focusable="false"
                />
              </a>
            </li>
          </ul>
          <div v-else class="recent-empty">
            <h3>{{ clientsCount === 0 ? copy.noDevices : copy.noHistory }}</h3>
            <p>{{ clientsCount === 0 ? copy.noDevicesHint : copy.noHistoryHint }}</p>
            <a href="/pin" class="home-text-action"
              >{{ clientsCount === 0 ? copy.pairFirst : copy.viewDevices
              }}<Icon
                name="chevron-right"
                :size="14"
                :stroke-width="1.8"
                icon-class="home-shortcut-arrow"
                aria-hidden="true"
                focusable="false"
            /></a>
          </div>
        </div>
      </section>
      <details v-if="showVersionDetails" id="version-details" class="home-version">
        <summary>
          <span class="home-version-label">{{ copy.versionDetails }}</span>
          <span class="home-version-current">
            <span>{{ $t('index.current_version') }}</span>
            <span class="home-version-value">{{ version?.version }}</span>
            <span class="home-version-dot" aria-hidden="true"></span>
          </span>
        </summary>
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
      </details>
      <footer class="home-footer home-enter" style="--enter-delay: 300ms">
        <nav class="home-footer-help" :aria-label="copy.quickStart">
          <a :href="tutorialHref" target="_blank" rel="noopener noreferrer">{{ copy.quickStart }}</a
          ><a
            :href="communityHref"
            target="_blank"
            rel="noopener noreferrer"
            @click.prevent="openCommunity"
            >{{ copy.community }}</a
          >
        </nav>
        <nav class="home-footer-links" :aria-label="$t('resource_card.legal')">
          <a :href="websiteHref" target="_blank" rel="noopener noreferrer">{{ copy.website }}</a
          ><a :href="OFFICIAL_RESOURCES[1].href" target="_blank" rel="noopener noreferrer">GitHub</a
          ><a
            v-for="resource in LEGAL_RESOURCES"
            :key="resource.id"
            :href="resource.href"
            target="_blank"
            rel="noopener noreferrer"
            >{{ $t(resource.titleKey) }}</a
          ><a
            class="home-sponsor-link"
            href="https://www.alkaidlab.com/#sponsors"
            target="_blank"
            rel="noopener noreferrer"
            >{{ copy.support.entry }}</a
          >
        </nav>
        <div class="home-credit">
          <a :href="websiteHref" target="_blank" rel="noopener noreferrer"
            >Powered by Alkaidlab（摇光）</a
          ><span>GNU GPL v3.0</span>
        </div>
      </footer>
    </main>
  </div>
</template>

<script setup>
import { computed, nextTick, onMounted, onBeforeUnmount, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import Navbar from '../components/layout/Navbar.vue'
import SetupWizard from '../components/SetupWizard.vue'
import ErrorLogs from '../components/common/ErrorLogs.vue'
import VersionCard from '../components/common/VersionCard.vue'
import Icon from '../components/common/Icon.vue'
import { useVersion } from '../composables/useVersion.js'
import { useLogs } from '../composables/useLogs.js'
import { useSetupWizard } from '../composables/useSetupWizard.js'
import { trackEvents } from '../config/firebase.js'
import { getBootstrapConfig } from '../config/bootstrapData.js'
import {
  OFFICIAL_RESOURCES,
  QUICK_START_RESOURCES,
  LEGAL_RESOURCES,
  resolveResourceHref,
  resolveRandomResourceHref,
} from '../config/resources.js'
import { getHomeCopy } from '../i18n/home.js'
import { apiJson } from '../utils/apiFetch.js'
import { openExternalUrl } from '../utils/helpers.js'
import { openNativeDisplaySettings } from '../utils/nativeHomeNavigation.js'
import {
  formatRelativeSeen,
  getStreamingDestination,
  getStreamingState,
  parseHomeClients,
  pruneRecentConnectionStores,
  readRecentConnections,
  updateRecentConnections,
  writeRecentConnections,
} from '../utils/recentConnections.js'

const { locale } = useI18n()
const copy = computed(() => getHomeCopy(locale.value))
const hostConfig = ref(null)
const hostNameEl = ref(null)
let hostNameObserver = null

// Poster-scale type must stay on one line: measure the rendered name and
// shrink it to fit before letting a long name wrap at the size floor. Short
// names keep the full 10vw display size.
function fitHostName() {
  const el = hostNameEl.value
  if (!el) return
  el.style.fontSize = ''
  el.style.whiteSpace = 'nowrap'
  const base = parseFloat(getComputedStyle(el).fontSize)
  const width = el.scrollWidth
  const available = el.parentElement.clientWidth
  if (!base || !width || !available) return
  let fitted = Math.floor(Math.max(40, Math.min(base, (base * available) / width)))
  el.style.fontSize = `${fitted}px`
  // One measured correction pass: rounding and glyph metrics can push the
  // prediction a hair off. Re-derive from the real rendered width once;
  // if even the corrected size overflows, fall back to wrapping.
  if (el.scrollWidth > available) {
    fitted = Math.max(40, Math.floor((fitted * available) / el.scrollWidth))
    el.style.fontSize = `${fitted}px`
    if (el.scrollWidth > available) el.style.whiteSpace = 'normal'
  }
}
watch(
  () => hostConfig.value?.sunshine_name,
  () => nextTick(fitHostName),
)
const hostStatus = ref('loading')
const appsCount = ref(null)
const clientsCount = ref(null)
const greetingText = computed(() =>
  clientsCount.value === 0
    ? copy.value.firstGreeting
    : copy.value.whisper[timeGreetingSlot(new Date().getHours())],
)
// Late-night streamers are a real audience: the whisper follows the clock.
function timeGreetingSlot(hour) {
  if (hour >= 22 || hour < 5) return 'lateNight'
  if (hour < 11) return 'morning'
  if (hour < 18) return 'afternoon'
  return 'evening'
}
const greetingHasCjk = computed(() => /[㐀-鿿]/u.test(greetingText.value))
// Full-width punctuation ends with half an empty em box; the greeting hangs it
// so that whitespace doesn't stack onto the flex gap before "Sunshine".
const greetingHangsPunct = computed(() => /[，。．！？…—]$/u.test(greetingText.value))
const clientsLoading = ref(true)
const clientsError = ref(false)
const clients = ref([])
const recentConnections = ref([])
const streamingClients = computed(
  () =>
    new Set(
      clients.value.filter((client) => client.streaming === true).map((client) => client.uuid),
    ),
)
const streamState = computed(() =>
  getStreamingState(clientsCount.value === null ? null : clients.value, clientsError.value),
)
const streamArrival = computed(() => getStreamingDestination(clients.value, copy.value))
const websiteHref = computed(() => resolveResourceHref(OFFICIAL_RESOURCES[0], locale.value))
const tutorialHref = computed(() => resolveResourceHref(QUICK_START_RESOURCES[0], locale.value))
const communityHref = QUICK_START_RESOURCES[1].href
const historyKey = () =>
  `sunshine-recent-connections:${window.location.host}:${hostConfig.value?.sunshine_name || ''}`
let refreshTimer
let reportTimer
let disposed = false
let refreshing = false
let clientsRequest = null
let firstRefresh = true

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
const showVersionDetails = computed(
  () =>
    buildVersionIsDirty.value ||
    installedVersionNotStable.value ||
    stableBuildAvailable.value ||
    (notifyPreReleases.value && preReleaseBuildAvailable.value),
)
const { fatalLogs, fetchLogs } = useLogs()
const { showSetupWizard, adapters, displayDevices, hasLocale, checkSetupWizard, onSetupComplete } =
  useSetupWizard()

function openDisplay(event) {
  if (openNativeDisplaySettings()) event.preventDefault()
}
async function openCommunity() {
  try {
    await openExternalUrl(resolveRandomResourceHref(QUICK_START_RESOURCES[1]))
  } catch (error) {
    console.error('Failed to open community:', error)
  }
}
function formatLastSeen(timestamp) {
  const relative = formatRelativeSeen(timestamp, copy.value.time)
  if (relative) return relative
  try {
    return new Intl.DateTimeFormat(String(locale.value).replace(/_/g, '-'), {
      month: 'short',
      day: 'numeric',
      hour: '2-digit',
      minute: '2-digit',
    }).format(timestamp)
  } catch {
    return new Date(timestamp).toLocaleString('en')
  }
}
async function refreshClients() {
  if (disposed || refreshing || showSetupWizard.value) return
  refreshing = true
  clientsLoading.value = true
  clientsRequest = new AbortController()
  let timedOut = false
  const timeout = setTimeout(() => {
    timedOut = true
    clientsRequest?.abort()
  }, 8000)
  try {
    const payload = await apiJson('/api/clients/list', { signal: clientsRequest.signal })
    if (disposed) return
    const list = parseHomeClients(payload)
    let previous = recentConnections.value
    if (firstRefresh) {
      try {
        previous = readRecentConnections(window.localStorage, historyKey())
        pruneRecentConnectionStores(window.localStorage, historyKey())
      } catch {
        previous = []
      }
    }
    firstRefresh = false
    clients.value = list
    clientsCount.value = list.length
    recentConnections.value = updateRecentConnections(previous, list)
    try {
      writeRecentConnections(window.localStorage, historyKey(), recentConnections.value)
    } catch {
      /* Storage may be disabled. */
    }
    clientsError.value = false
  } catch (error) {
    if (!disposed && (timedOut || error.name !== 'AbortError')) clientsError.value = true
  } finally {
    clearTimeout(timeout)
    refreshing = false
    if (!disposed) clientsLoading.value = false
  }
}
function refreshVisibleClients() {
  if (!document.hidden) void refreshClients()
}
onBeforeUnmount(() => {
  disposed = true
  clientsRequest?.abort()
  clearInterval(refreshTimer)
  clearTimeout(reportTimer)
  document.removeEventListener('visibilitychange', refreshVisibleClients)
  hostNameObserver?.disconnect()
})
onMounted(async () => {
  trackEvents.pageView('home')
  try {
    const config = await getBootstrapConfig()
    if (disposed) return
    hostConfig.value = config
    hostStatus.value = 'ready'
    if (checkSetupWizard(config)) return
    await nextTick()
    fitHostName()
    hostNameObserver = new ResizeObserver(fitHostName)
    // Observe the full-width grid, not the fit-content summary: the summary
    // tracks its content width and would never re-fire on viewport resizes.
    const fitScope = hostNameEl.value?.closest('.host-overview') ?? hostNameEl.value?.parentElement
    if (fitScope) hostNameObserver.observe(fitScope)
    reportTimer = setTimeout(() => {
      const gpuAdapters = config.adapters || []
      trackEvents.gpuReported({
        platform: config.platform || 'unknown',
        adapter_count: gpuAdapters.length,
        adapters: gpuAdapters.map((a) => (typeof a === 'string' ? a : a?.name || '')).join(', '),
        selected_adapter: config.adapter_name || (gpuAdapters.length ? 'auto' : 'none'),
        has_selected_adapter: !!config.adapter_name,
      })
    }, 1000)
    document.addEventListener('visibilitychange', refreshVisibleClients)
    refreshTimer = setInterval(refreshVisibleClients, 10000)
    void refreshClients()
    void apiJson('/api/apps')
      .then((payload) => {
        if (!disposed)
          appsCount.value = Array.isArray(payload)
            ? payload.length
            : Array.isArray(payload?.apps)
              ? payload.apps.length
              : null
      })
      .catch(() => {})
    await Promise.allSettled([fetchVersions(config), fetchLogs()])
    if (!disposed && version.value) document.title += ` Ver ${version.value.version}`
  } catch (error) {
    if (disposed) return
    hostStatus.value = 'error'
    clientsError.value = true
    clientsLoading.value = false
    console.error('Failed to initialize home:', error)
  }
})
</script>

<style>
@import '../styles/global.less';
</style>
<style scoped src="../styles/home.less" lang="less"></style>
