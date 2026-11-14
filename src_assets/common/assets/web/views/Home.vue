<template>
  <div class="home-page">
    <MPage :title="$t('home.dash.title')" wide full>
      <template #actions>
        <ShellUtils placement="bar" />
        <MButton v-if="edit" variant="tonal" icon="check" @click="edit = false">{{ $t('home.grid.done') }}</MButton>
        <MIconButton v-else icon="edit" data-coach="home-edit" :label="$t('home.grid.edit')" @click="edit = true" />
      </template>

      <MBanner v-if="hostStatus === 'error'" tone="error" icon="error">
        <b>{{ $t('_common.error') }}</b> {{ $t('index.startup_errors').replace(/<[^>]+>/g, '') }}
      </MBanner>
      <!-- the host's startup problems as a compact card: one short line each, the original message behind a toggle, the log and (where known) the fix at the trailing end -->
      <MStartupProblems v-if="startup.length" :problems="startup" :locale="String(locale)" @logs="go('troubleshooting')" @fix="onStartupFix" />

      <MAnnouncementStrip v-if="announcements.top.value" :item="announcements.top.value" :total="announcements.unread.value" :texts="announceTexts" @dismiss="announcements.dismiss" @open-url="openExternalUrl" @open-list="announceOpen = true" />
      <MHomeDash v-model:edit="edit" :cards="cards" :details="allDetails" :ctx="dashCtx" storage-key="web-home" :texts="dashTexts" :config="hostConfig" :locale="locale" @action="onAction" />
      <MSection v-if="session" class="home-live-controls" :title="$tp('config.host_audio', 'Host audio')">
        <MGroup>
          <MItem icon="volume_up" :title="$tp('config.host_audio', 'Play audio on host')" :text="$tp('config.host_audio_desc', 'Apply to the current stream without reconnecting when supported.')">
            <template #trail><MSwitch :model-value="session.host_audio === true" :disabled="audioBusy" :label="$tp('config.host_audio', 'Host audio')" @update:model-value="setHostAudio" /></template>
          </MItem>
        </MGroup>
      </MSection>
    </MPage>

    <!-- Update sheet: current -> new, channel, driver notice, short release notes, link to the release (this page cannot install) -->
    <VersionCard
      v-model="versionSheet"
      :version="version"
      :github-version="githubVersion"
      :pre-release-version="preReleaseVersion"
      :notify-pre-releases="notifyPreReleases"
      :loading="loading"
      :check-failed="versionCheckStatus === VERSION_CHECK_STATUS.ERROR"
      :installed-version-not-stable="installedVersionNotStable"
      :stable-build-available="stableBuildAvailable"
      :pre-release-build-available="preReleaseBuildAvailable"
      :build-version-is-dirty="buildVersionIsDirty"
      @retry="fetchVersions(hostConfig)"
    />
    <MAnnouncementSheet v-model="announceOpen" :items="announcements.list.value" :title="homeTexts(locale).annTitle" :texts="announceTexts" :close-label="$t('_common.close')" @dismiss="announcements.dismiss" @open-url="openExternalUrl" />
    <MSupportSheet v-model="sponsorSheet" :title="$t('home.dash.sponsor')" :sections="supportSections" :close-label="$t('_common.close')" @open="onSupportOpen" />
    <ResourceSheet v-model="linksSheet" />

    <ConfirmDialog :show="stopAsk" dialog-id="home-stop-session" :title="$t('home.stop_session')" title-icon="warning" tone="danger" :close-label="$t('_common.close')" @close="stopAsk = false">
      <p>{{ $t('home.stop_body') }}</p>
      <template #actions>
        <MButton variant="text" @click="stopAsk = false">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" @click="stopSessions">{{ $t('home.stop_session') }}</MButton>
      </template>
    </ConfirmDialog>
    <ConfirmDialog :show="restartAsk" dialog-id="home-restart" :title="$t('troubleshooting.restart_sunshine')" title-icon="refresh" tone="danger" :close-label="$t('_common.close')" @close="restartAsk = false">
      <p>{{ $t('troubleshooting.restart_sunshine_desc') }}</p>
      <template #actions>
        <MButton variant="text" @click="restartAsk = false">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" icon="refresh" @click="restartHost">{{ $t('troubleshooting.restart_sunshine') }}</MButton>
      </template>
    </ConfirmDialog>
    <MSnackbar v-model="notice" :message="noticeText" />
  </div>
</template>

<script setup>
import { computed, inject, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { firstImage, recentErrorCount, micIsReady, startupProblems, MStartupProblems, MBanner, MButton, MIcon, MIconButton, MItem, MGroup, MPage, MSection, MSheet, MSnackbar, MSwitch, MHomeDash, MSupportSheet, MAnnouncementStrip, MAnnouncementSheet, useAnnouncements, useGuideCard, buildHomeModel, buildStartSteps, buildCards, useJourneyHome, useReleaseAssets, useStarCard, useSystemInfo, reachProblemOf, registeredComponentStatus, guideCardData, downloadCardData, sponsorCardData, GUIDE_TARGET, homeTexts, parseErrorLog, parseSessionLog } from 'foundation-ui/vue'
import { realSession, buildCatalogue, buildCatalogueDetails, createHistory, createPairQr, saveSettingKey, recentKeys, rememberRecent, createQueuedCalls, createSerialCalls } from 'foundation-ui/vue'
import QRCode from 'qrcode'
import { useVddSettings } from '../composables/useVddSettings.js'
import { useSharedConfig } from '../composables/configStore.js'
import VersionCard from '../components/common/VersionCard.vue'
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import ResourceSheet from '../components/common/ResourceSheet.vue'
import ShellUtils from '../components/layout/ShellUtils.vue'
import { VERSION_CHECK_STATUS, useVersion } from '../composables/useVersion.js'
import { useLogs } from '../composables/useLogs.js'
import { useSetupWizard } from '../composables/useSetupWizard.js'
import { useReach } from '../composables/useReach.js'
import { useHostLive } from '../composables/useHostLive.js'
import { canOpenBigScreen, openBigScreenOnHost } from '../composables/openBigScreen.js'
import { useShellLive } from '../composables/useShellLive.js'
import { trackEvents } from '../config/telemetry.js'
import { hostData } from '../composables/hostData.js'
import { SPONSORS } from '../config/sponsors.js'
import { deviceIconFor } from '../utils/homeData.js'
import { openExternalUrl } from '../utils/helpers.js'
import { getImagePreviewUrl } from '../utils/imageUtils.js'
import { ds4HintOf } from '../utils/gamepadHint.js'
import { apiFetch, apiJson, apiPostJson } from '../utils/apiFetch.js'
import { $tp, setPlatformHint } from '../platform-i18n'

const { t, locale } = useI18n()
const webShell = inject('webShell', null)
const hostConfig = ref(null)
const hostStatus = ref('loading')
const edit = ref(false)
const T = computed(() => homeTexts(locale.value))

const platform = computed(() => hostConfig.value?.platform)
// the platform variants of a text ($tp) need the platform wherever they are asked, also in a callback outside of rendering
watch(platform, (value) => setPlatformHint(value), { immediate: true })
const announcements = useAnnouncements({ locale, version: computed(() => hostConfig.value?.version) })
const announceOpen = ref(false)
const announceTexts = computed(() => { const x = homeTexts(locale.value); return { open: x.annOpen, dismiss: x.annDismiss, all: x.annAll, read: x.annRead, none: x.annNone } })
const WANT = new Set(['session', 'latency', 'bitrate', 'fps', 'hdr', 'mic', 'vdd', 'cpu', 'gpu', 'memory'])
const live = useHostLive({ want: () => WANT, platform: () => platform.value })
const go = (id) => (webShell?.nav ? webShell.nav.go(id) : (window.location.href = id === 'home' ? '/' : `/${id}`))
const pairClient = () => { go('pin'); if (webShell) webShell.pending.value = 'pair' }

// ---- Data ----
const shellLive = useShellLive()
// the session with the real name of its client (realSession), and its measured bitrate from either route of the host (sessions[] or perf/current); `bitrate` stays the budget
const rawSession = computed(() => realSession(live.sessions.value[0] || null, shellLive.clients.value?.list))
const session = computed(() => { const s = rawSession.value; if (!s) return s; const m = Number.isFinite(s.measured_bitrate_kbps) ? s.measured_bitrate_kbps : live.perf.value?.measuredKbps; return Number.isFinite(m) ? { ...s, measured_bitrate_kbps: m } : s })
const audioBusy = ref(false)
async function setHostAudio(enabled) {
  const s = session.value
  if (!s || audioBusy.value) return
  audioBusy.value = true
  try {
    const d = await apiPostJson('/api/runtime/audio', { clientname: s.client_name, enabled })
    if (!d?.success) throw new Error(d?.status_message || 'host audio')
    await live.refreshAux?.()
  } catch { say($tp('config.host_audio_failed', 'Host audio could not be changed.')) } finally { audioBusy.value = false }
}
const bitrateSample = (s) => (s && Number.isFinite(s.measured_bitrate_kbps) ? s.measured_bitrate_kbps : s?.bitrate)
const perf = live.perf
const sys = live.system
const pushSample = (list, v) => { if (Number.isFinite(v)) list.value = [...list.value.slice(-59), v] }
const series = { cpu: ref([]), gpu: ref([]), bitrate: ref([]), fps: ref([]) }
watch(sys, (s) => { if (s) { pushSample(series.cpu, s.cpu_percent); pushSample(series.gpu, s.gpu_percent) } })
watch(() => [bitrateSample(session.value), perf.value?.fps], ([b, f]) => { if (session.value) { pushSample(series.bitrate, b); pushSample(series.fps, f) } else { series.bitrate.value = []; series.fps.value = [] } })

// history this interface keeps itself (the host has none): finished sessions and minutes per day, plus samples the curves need
const history = createHistory(localStorage, 'foundation-ui.history.web-home')
const historyTick = ref(0)
watch(session, (s) => { if (history.observe(s ? { ...s, fpsMeasured: perf.value?.fps, latencyP95Ms: perf.value?.p95Ms } : null)) historyTick.value += 1 }, { immediate: true })
// one sample of every live number every three seconds, kept for an hour (the time ranges of the detail sheets read it)
const SAMPLE_SEC = 3
function sampleAll() {
  const s = session.value, sy = sys.value
  history.sample('cpu', sy?.cpu_percent); history.sample('gpu', sy?.gpu_percent)
  if (sy?.memory_total) history.sample('mem', ((sy.memory_total - (sy.memory_available || 0)) / sy.memory_total) * 100)
  if (s) { history.sample('bitrate', bitrateSample(s)); history.sample('fps', perf.value?.fps); history.sample('latency', perf.value?.avgMs); history.sample('loss', s.packet_loss); history.sample('dropped', s.dropped_frames) } else for (const k of ['bitrate', 'fps', 'latency', 'loss', 'dropped']) history.clear(k)
  historyTick.value += 1
}
let sampler = 0
onMounted(() => { sampleAll(); sampler = setInterval(sampleAll, SAMPLE_SEC * 1000) })
onBeforeUnmount(() => clearInterval(sampler))

const apps = ref(null)
async function fetchApps() {
  try {
    const data = await apiJson('/api/apps')
    const list = Array.isArray(data) ? data : data?.apps
    apps.value = Array.isArray(list) ? list.map((a) => ({ id: a.uuid || a.name, name: a.name, cover: a['image-path'] ? getImagePreviewUrl(a['image-path']) : '' })) : null
  } catch { apps.value = null }
}

// gamepad type: the host's controller configuration, applied to the next controller a client creates (no restart)
const gamepadMode = ref('')
const gamepadBusy = ref(false)
async function fetchGamepad() {
  try {
    const data = await apiJson('/api/gamepad/config')
    gamepadMode.value = data?.status === 'true' && ['auto', 'x360', 'ds4', 'ds5'].includes(data.gamepad) ? data.gamepad : ''
  } catch { gamepadMode.value = '' }
}
async function setGamepad(value) {
  if (gamepadBusy.value || value === gamepadMode.value) return
  const before = gamepadMode.value
  gamepadMode.value = value
  gamepadBusy.value = true
  try {
    const data = await apiPostJson('/api/gamepad/config', { gamepad: value })
    if (data?.status !== 'true') throw new Error('not saved')
    say(t('home.dash.pad_saved'))
  } catch { gamepadMode.value = before; say(t('_common.error')) } finally { gamepadBusy.value = false }
}

const hdrState = computed(() => {
  const h = live.hdr.value
  if (!h) return ''
  const key = `home.hdr_state.${h.state}`
  const text = t(key)
  return text === key ? String(h.state || '') : text
})
const vddState = computed(() => {
  const v = live.vdd.value
  if (!v) return ''
  const key = `home.vdd_state.${v.state}`
  const text = t(key)
  return text === key ? String(v.state || '') : text
})
const micText = computed(() => {
  const m = live.mic.value
  return m ? t(m.host_streaming ? 'home.mic_in_use' : m.online ? 'home.mic_online' : 'home.mic_offline') : ''
})

const { logs, fancyLogs, fatalLogs, fetchLogs } = useLogs()
const startup = computed(() => startupProblems(fatalLogs.value.map((log) => log.value), String(locale.value)))
// the fix of a known problem: the driver page of this UI, else the page of the project's documents that names it
const onStartupFix = (problem) => {
  const known = problem.known || {}
  if (known.go === 'drivers') go('components')
  else if (known.link) openExternalUrl(known.link)
  else if (known.go) go('config')
}

// ---- Get started card: steps tick from real data ----
const SEEN_KEY = 'foundation-ui.guide.web-home.sessionSeen'
const sessionSeen = ref(localStorage.getItem(SEEN_KEY) === '1')
watch(() => !!session.value || (parseSessionLog(logs.value)?.length || 0) > 0, (v) => { if (v && !sessionSeen.value) { sessionSeen.value = true; try { localStorage.setItem(SEEN_KEY, '1') } catch { /* storage unavailable */ } } }, { immediate: true })
const guide = useGuideCard('web-home', () => (shellLive.clients.value && apps.value ? buildStartSteps(T.value, { paired: shellLive.clients.value.total, sessionSeen: sessionSeen.value, apps: apps.value }) : []))
const openGuide = async () => (await import('../utils/guideLauncher.js')).openGuide()
const downloadFor = (id) => { window.location.href = `/downloads?platform=${encodeURIComponent(id)}` }

// ---- Host status and version (unchanged behaviour of the old home) ----
const {
  version, githubVersion, preReleaseVersion, notifyPreReleases, versionCheckStatus, loading, installedVersionNotStable,
  stableBuildAvailable, preReleaseBuildAvailable, buildVersionIsDirty, fetchVersions,
} = useVersion()
const versionSheet = ref(false)
const hasStableUpdate = computed(() => stableBuildAvailable.value)
const hasPreReleaseUpdate = computed(() => notifyPreReleases.value && preReleaseBuildAvailable.value)
const updateLabel = computed(() => (hasStableUpdate.value ? t('index.new_stable') : hasPreReleaseUpdate.value ? t('index.new_pre_release') : ''))
const versionLabel = computed(() => {
  const current = version.value?.version || hostConfig.value?.version
  return current ? `Ver ${current}` : ''
})

// ---- The model of the shared dashboard ----
const dashTexts = computed(() => ({ ...homeTexts(locale.value), done: t('home.grid.done'), reset: t('home.grid.reset'), earlier: t('home.grid.earlier'), later: t('home.grid.later'), addTitle: t('home.grid.add'), nothing: t('home.grid.nothing'), hide: t('home.grid.remove'), close: t('_common.close') }))
const PAD_LABELS = computed(() => ({ auto: t('home.dash.pad_auto'), x360: 'Xbox 360', ds4: 'DualShock 4', ds5: 'DualSense' }))
const PLATFORM_NAMES = { windows: 'Windows', linux: 'Linux', macos: 'macOS' }
// the pad a streaming client really runs: the host reports the active type of the session's PlayStation controller
const padSession = computed(() => live.sessions.value.find((s) => s && s.gamepad_type) || null)
// one short line with the reason and what to do while a client's PlayStation pad runs as a DualShock 4
const ds4Hint = computed(() => ds4HintOf(live.sessions.value, gamepadMode.value))
const padLabel = computed(() => {
  const type = padSession.value?.gamepad_type
  if (type) return PAD_LABELS.value[type] || type
  return gamepadMode.value ? PAD_LABELS.value[gamepadMode.value] : ''
})
const model = computed(() => {
  const s = session.value
  const sy = sys.value
  const base = buildHomeModel({
    host: { name: hostConfig.value?.sunshine_name || 'Sunshine', platform: PLATFORM_NAMES[platform.value] || '', version: versionLabel.value, running: hostStatus.value !== 'error', update: updateLabel.value },
    session: s ? { ...s, fpsMeasured: perf.value?.fps, latencyMs: perf.value?.avgMs, latencyP95Ms: perf.value?.p95Ms, latencyLastMs: perf.value?.lastMs, capture: perf.value?.capture, uptimeMs: perf.value?.uptimeMs } : null,
    sessionCount: live.sessions.value.length,
    series: { cpu: series.cpu.value, gpu: series.gpu.value, bitrate: series.bitrate.value, fps: series.fps.value, latency: perf.value?.series || [] },
    codecModes: hostConfig.value && hostConfig.value.hevc_mode !== undefined ? { hevc: hostConfig.value.hevc_mode, av1: hostConfig.value.av1_mode } : null,
    sys: sy ? { cpu: sy.cpu_percent, gpu: sy.gpu_percent, gpuName: sy.gpu_name || '', memTotal: sy.memory_total || 0, memAvail: sy.memory_available || 0 } : null,
    encoder: shellLive.encoder.value,
    vdd: live.vdd.value ? { state: live.vdd.value.state === 'ready' ? 'ready' : (live.vdd.value.state === 'missing' || live.vdd.value.state === 'not_installed') ? 'missing' : 'idle', text: vddState.value, hdr: live.hdr.value ? !!live.hdr.value.hdrEnabled : undefined } : null,
    display: hdrState.value || vddState.value ? { hdr: live.hdr.value ? !!live.hdr.value.hdrEnabled : undefined, hdrText: hdrState.value, nits: live.hdr.value?.calibrated ? live.hdr.value.maxNits : null, vddText: vddState.value, vddTone: live.vdd.value?.state === 'ready' ? 'ok' : 'warn' } : null,
    pads: padLabel.value ? { word: padLabel.value, text: ds4Hint.value ? `${padLabel.value} · ${t(ds4Hint.value.key)}` : padLabel.value } : null,
    mic: micText.value ? { ready: micIsReady(live.mic.value), text: micText.value, sub: live.mic.value?.underruns ? t('home.underruns', { n: live.mic.value.underruns }) : '' } : null,
    clients: shellLive.clients.value?.list ? shellLive.clients.value.list.map((c) => ({ ...c, icon: deviceIconFor(c.name, c.os) })) : null,
    apps: apps.value,
    logs: parseErrorLog(logs.value),
    recent: parseSessionLog(logs.value),
    canAct: live.local.value !== false || !s,
  }, homeTexts(locale.value))
  base.strip = gamepadMode.value
    ? { label: t('home.dash.gamepad_type'), icon: 'sports_esports', value: gamepadMode.value, busy: gamepadBusy.value, options: ['auto', 'x360', 'ds4', 'ds5'].filter((m) => m !== 'ds4' && m !== 'x360' ? true : platform.value !== 'linux').map((value) => ({ value, label: PAD_LABELS.value[value] })), switches: [] }
    : null
  if (base.strip && ds4Hint.value) base.strip.note = t(ds4Hint.value.key)
  // the version sheet is the details of the host card (release notes, update channels)
  base.details = { ...base.details, hero: { external: true } }
  return base
})
// the shared virtual display control and the store the settings page uses, so a change in one shows in the other
const vddStore = useVddSettings()
onMounted(() => { const shared = useSharedConfig(); if (!shared.config.value) shared.loadConfig(); vddStore.refresh() })
const catalogueInput = computed(() => {
  void historyTick.value
  const s = session.value
  const sy = sys.value
  return {
    sampleSec: SAMPLE_SEC, vddStore: live.vdd.value ? vddStore : null,
    reach: reach.view.report, reachChecking: reach.view.status === 'loading',
    sysinfo: nics.info.value,
    host: { version: versionLabel.value, update: updateLabel.value },
    session: s ? { ...s, fpsMeasured: perf.value?.fps, latencyMs: perf.value?.avgMs, latencyP95Ms: perf.value?.p95Ms, uptimeMs: perf.value?.uptimeMs } : null,
    history: { samples: history.samples, sessions: history.sessions, days: history.days, last: () => history.last() },
    sys: sy ? { memTotal: sy.memory_total || 0, memAvail: sy.memory_available || 0 } : null,
    config: hostConfig.value, logText: logs.value, logs: parseErrorLog(logs.value), clients: model.value.table?.tabs?.find((x) => x.id === 'clients') ? shellLive.clients.value.list.map((c) => ({ ...c, icon: deviceIconFor(c.name, c.os) })) : null, apps: apps.value,
    codecModes: hostConfig.value && hostConfig.value.hevc_mode !== undefined ? { hevc: hostConfig.value.hevc_mode, av1: hostConfig.value.av1_mode } : null,
    pads: padLabel.value ? { word: padLabel.value, text: ds4Hint.value ? `${padLabel.value} · ${t(ds4Hint.value.key)}` : padLabel.value } : null,
    display: live.hdr.value ? { hdr: !!live.hdr.value.hdrEnabled, hdrText: hdrState.value, nits: live.hdr.value.calibrated ? live.hdr.value.maxNits : null } : null,
    canAct: live.local.value !== false || !s,
    canOpenDesktop: canOpenBigScreen({ platform: platform.value, local: live.local.value, hostname: window.location.hostname }),
    recent: recentKeys.value, canRuntime: live.local.value !== false, ds5Keys: shellLive.dualsenseKeys.value,
    pairQr: pairQr.view,
    announcements: announcements.list.value.map((a) => ({ id: a.id, title: a.title || a.text || '', category: a.category, banner: a.banner || firstImage(a.body), date: dateOfPost(a.published), read: a.read || a.snoozed, hasText: !!a.body, url: a.url || '' })),
  }
})
const catalogue = computed(() => buildCatalogue(catalogueInput.value, T.value).data)
const allDetails = computed(() => ({ ...model.value.details, ...buildCatalogueDetails(catalogueInput.value, T.value) }))
const isNew = computed(() => guide.visible.value)
const cards = computed(() => buildCards(model.value, { texts: T.value, catalogue: catalogue.value, guide: guideCardData(T.value, guide.steps.value, guide.visible.value), download: downloadCardData(T.value, { locale: locale.value, assetsOf: releases.assetsOf, versionOf: releases.versionOf, qr: (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules }), sponsor: sponsorCardData(T.value, SPONSORS), journey: journey.data.value, sysinfo: nics.info.value, temps: null }))
// the system information of the host (the network adapters, the CPU, the GPU, the system): a host without the API leaves those tiles out
const sysJson = async (path) => { const r = await apiFetch(path); if (!r.ok) throw Object.assign(new Error(`HTTP ${r.status}`), { status: r.status }); return r.json() }
const nics = useSystemInfo({ fetchJson: sysJson })
const reachProblem = computed(() => reachProblemOf(reach.view.report))
// what the smart overview reads (all local): the paired clients, a stream, problems, the network of the host
const compRows = () => registeredComponentStatus()?.rows?.value || []
const smartSignals = computed(() => ({
  pairedCount: shellLive.clients.value?.list ? shellLive.clients.value.list.length : null,
  streaming: !!session.value, lastStreamAt: (void historyTick.value, history.last()?.end || 0), // ms since epoch of the end of the last finished session this page saw (0: none): the after-stream scenario runs 30 minutes from it
  problems: startup.value.length, problemComponents: compRows().filter((r) => r.state === 'error' || r.state === 'warn').map((r) => r.id),
  recentErrors: recentErrorCount(logs.value), isNew: isNew.value,
  wifiHost: nics.defaultNic.value?.type === 'wifi', linkSlowMbps: nics.defaultNic.value?.link_speed_mbps && nics.defaultNic.value.link_speed_mbps < 1000 ? nics.defaultNic.value.link_speed_mbps : 0,
  reachBad: reachProblem.value, updateAvailable: !!updateLabel.value,
}))
const dashCtx = computed(() => ({ live: !!session.value, isNew: isNew.value, ...journey.ctx.value, star: star.visible.value, starNames: (shellLive.clients.value?.list || []).map((c) => c.name), clientList: (shellLive.clients.value?.list || []).map((c) => ({ name: c.name, uuid: c.uuid, family: c.family, official: c.official, os: c.os, version: c.version, last_seen: c.last_seen })), sessionClient: session.value ? { uuid: session.value.client_cert_uuid || session.value.uuid || session.value.client_uuid || '', name: session.value.client_name || '', family: session.value.client_family || '', os: session.value.client_os || '', official: session.value.client_official } : null, signals: smartSignals.value }))
// the date of a post in the 资讯 tile, YYYY-MM-DD
const dateOfPost = (at) => { if (!at) return ''; const d = new Date(at); const p = (n) => String(n).padStart(2, '0'); return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}` }
function onAction(a) {
  switch (a.type) {
    case 'news-open': webShell?.nav ? webShell.nav.go('announcements', { id: a.value?.id }) : go('home'); break
    case 'hero': onHero(); break
    case 'group': setGamepad(a.value); break
    case 'table-action': pairClient(); break
    case 'pair-pin': pairPin(a.value?.pin); break
    case 'pair-qr': go('pin'); if (webShell) webShell.pending.value = 'pair-qr'; break
    case 'star-dismiss': star.dismiss(); break
    case 'address-save': saveSetting({ key: 'external_endpoints', value: a.value?.value ?? '' }).then(() => saveSetting({ key: 'external_ip', value: '' })).then(() => reach.refresh(true)).catch(() => {}); break
    case 'reach-check': reach.refresh(true); break
    case 'pin-open': webShell?.openSetting?.(a.value?.key); break
    case 'open-url': window.open(a.value, '_blank', 'noopener,noreferrer'); break
    case 'open-desktop': openBigScreenOnHost().catch((e) => say(`${T.value.openDesktopFailed}${e?.message ? `: ${e.message}` : ''}`)); break
    case 'pair-qr-begin': pairQr.begin(); break
    case 'pair-qr-stop': pairQr.stop(); break
    case 'table-row': onTableRow(a.value); break
    case 'app': go('apps'); break
    case 'add-app': go('apps'); if (webShell) webShell.pending.value = 'addApp'; break
    case 'scan-games': go('apps'); if (webShell) webShell.pending.value = 'scanGames'; break
    case 'open': go({ vdd: 'config', settings: 'config', clients: 'pin', diagnostics: 'troubleshooting' }[a.value] || a.value); break
    case 'detail-external': versionSheet.value = true; break
    case 'guide-go': { const to = GUIDE_TARGET[a.value.id]; if (to === 'welcome') openGuide(); else if (to === 'clients') pairClient(); else go(to === 'downloads' ? 'downloads' : to) } break
    case 'guide-skip': guide.toggle(a.value); break
    case 'download': downloadFor(a.value); break
    case 'link': window.open(a.value, '_blank', 'noopener,noreferrer'); break
    case 'setting': saveSetting(a.value); break
    case 'runtime-bitrate': runtimeBitrate(a.value); break
    case 'ds5-tune': ds5Tune(a.value); break
    case 'sponsor': { const target = SPONSORS.find((x) => x.id === a.value); if (a.value === 'star') window.open('https://github.com/AlkaidLab/foundation-sunshine', '_blank', 'noopener,noreferrer'); else if (target) window.open(target.url, '_blank', 'noopener,noreferrer'); else sponsorSheet.value = true; break }
    default: break
  }
}
// a control card saves its setting the way the settings do: read the configuration, change the key, write it back
const hostApi = { getJson: (path) => apiJson(path), postJson: (path, body) => apiJson(path, { method: 'POST', body }) }
// the QR code of the pairing card: a pairing window is open on the host only while the card shows it (the card asks and gives it back)
const pairQr = createPairQr({
  start: async () => { const d = await apiJson('/api/qr-pair', { method: 'POST' }); if (String(d.status) !== 'true') throw new Error(d.error || 'qr'); return d },
  poll: () => apiJson('/api/qr-pair'),
  cancel: () => apiFetch('/api/qr-pair/cancel', { method: 'POST' }),
  render: (url) => QRCode.toDataURL(url, { width: 280, margin: 2, color: { dark: '#000000', light: '#ffffff' } }),
})
onBeforeUnmount(() => pairQr.stop())
// the connect journey card: on by default while no client is paired (choose the device, install, pair, done)
const releases = useReleaseAssets()
const journey = useJourneyHome({ storageKey: 'web-home', assetsOf: releases.assetsOf, versionOf: releases.versionOf, count: () => (shellLive.clients.value?.list ? shellLive.clients.value.list.length : null), latest: () => (shellLive.clients.value?.list?.length ? shellLive.clients.value.list[shellLive.clients.value.list.length - 1].name : ''), locale: () => locale.value, qr: (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules, pairQr: pairQr.view })
// the star card: due after the first pairing or the first stream; while a stream runs the star of the client in use, otherwise the host and the sponsor entry; 以后再说 hides it until the next stream
const star = useStarCard({ storageKey: 'web-home', triggered: () => journey.done.value || sessionSeen.value, paired: () => shellLive.clients.value?.list?.length || 0, streaming: () => !!session.value })
// the PIN typed on the pairing card goes to the host like the pairing sheet sends it
async function pairPin(pin) {
  if (!/^\d{4}$/.test(String(pin || ''))) return
  try {
    const response = await apiFetch('/api/pin', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ pin: String(pin), name: T.value.pairDefaultName }) })
    const data = await response.json()
    say(String(data.status) === 'true' ? T.value.pairSent : T.value.pairFailed)
  } catch { say(T.value.pairFailed) }
}
// a control the keyboard repeats (a slider held on an arrow key) posts one command per destination at a time; the value that is sent is always the last one chosen
const saveSetting = createSerialCalls(async ({ key, value, failed }) => { const r = await saveSettingKey(hostApi, key, value); if (r.ok) { hostConfig.value = r.config; rememberRecent(key) } else { say(T.value.saveFailed); failed?.() } }, (a) => a.key)
// what acts on a running stream (host audit 2026-11-05): the bitrate command is taken from this computer only; the DualSense tuning applies to the controller in use
const runtimeBitrate = createQueuedCalls(async ({ client, kbps }) => {
  try { const r = await apiFetch(`/api/runtime/bitrate?bitrate=${kbps}&clientname=${encodeURIComponent(client)}`); const d = await r.json().catch(() => ({})); say(r.ok && d.success !== false ? T.value.lbSent.replace('{client}', client) : T.value.lbFailed) } catch { say(T.value.lbFailed) }
}, (a) => a.client)
const ds5Tune = createQueuedCalls(async ({ key, value }) => {
  try { const d = await apiPostJson('/api/dualsense/tuning', { schema: 'ALK-DS5T/1', values: { [key]: value } }); if (d?.status === false) say(T.value.dsFailed); else shellLive.refresh?.() } catch { say(T.value.dsFailed) }
}, (a) => a.key)
const onTableRow = (row) => go(row.tab === 'errors' || row.tab === 'sessions' ? 'troubleshooting' : 'pin')
const onDetail = (id) => { if (id === 'hero') versionSheet.value = true }

// ---- Sheets, confirmations ----
const sponsorSheet = ref(false)
const linksSheet = ref(false)
const supportSections = computed(() => [
  { title: t('home.dash.sponsor'), items: SPONSORS.map((x) => ({ id: x.id, icon: 'favorite', title: x.label, url: x.url })) },
  { items: [
    { id: 'star', icon: 'star', title: t('home.dash.star'), url: 'https://github.com/AlkaidLab/foundation-sunshine' },
    { id: 'qq', icon: 'groups', title: t('home.dash.qq'), url: 'https://qm.qq.com/q/AfMQoyKrkc' },
    { id: 'more', icon: 'public', title: t('home.dash.more_resources') },
  ] },
])
const onSupportOpen = (item) => {
  if (item.id === 'more') { sponsorSheet.value = false; linksSheet.value = true; return }
  window.open(item.url, '_blank', 'noopener,noreferrer')
}
const stopAsk = ref(false)
const restartAsk = ref(false)
const notice = ref(false)
const noticeText = ref('')
const say = (text) => { noticeText.value = text; notice.value = true }
const onHero = () => { if (session.value) stopAsk.value = true; else restartAsk.value = true }
async function stopSessions() {
  stopAsk.value = false
  try {
    const response = await apiFetch('/api/runtime/sessions/stop', { method: 'POST' })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    say(t('home.stop_done'))
  } catch (error) { console.error('Failed to stop sessions:', error); say(t('_common.error')) }
}
async function restartHost() {
  restartAsk.value = false
  try {
    await apiFetch('/api/restart', { method: 'POST' })
    say(t('troubleshooting.restart_sunshine_success'))
  } catch (error) {
    // the host drops the connection while it restarts
    say(t('troubleshooting.restart_sunshine_success'))
    console.warn('Restart request ended:', error)
  }
}

// The first run is the guided flow (the same one as the control panel's welcome), a large window over a scrim
const { checkSetupWizard } = useSetupWizard()
const reach = useReach()

let logTimer = 0
let appsTimer = 0
onMounted(async () => {
  void reach.refresh()
  nics.start()
  shellLive.start()
  void fetchApps()
  void fetchGamepad()
  appsTimer = setInterval(() => { if (!document.hidden) void fetchApps() }, 30000)
  try {
    const config = await hostData.refresh('config')
    hostConfig.value = config
    hostStatus.value = 'ready'
    if (checkSetupWizard(config)) void import('../utils/guideLauncher.js').then((m) => m.maybeAutoOpenGuide({ ignorePaired: true }))
    // version and logs do not block each other, so an external version check cannot delay the local status
    await Promise.allSettled([fetchVersions(config), fetchLogs()])
    logTimer = setInterval(() => { if (!document.hidden) void fetchLogs() }, 30000)
    if (version.value) document.title += ` Ver ${version.value.version}`
  } catch (e) {
    hostStatus.value = 'error'
    // in preview mode the API is not there; that is expected
    if (e?.message?.includes('JSON') || e?.message?.includes('<!DOCTYPE')) console.warn('API not available in preview mode:', e.message)
    else {
      console.error('Failed to initialize:', e)
      trackEvents.errorOccurred('home_initialization', e.message)
    }
  }
})
onBeforeUnmount(() => { clearInterval(logTimer); clearInterval(appsTimer); shellLive.stop() })
</script>
