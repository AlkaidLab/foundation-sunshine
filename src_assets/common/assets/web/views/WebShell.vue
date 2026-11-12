<template>
  <div v-if="embedded" class="m-app m-web-embedded">
    <MPinHost ref="pinHost" storage-key="web-home" :locale="String(uiLocale)" :forward="forwardPin"><component :is="VIEWS[embeddedId]" :key="embeddedId" /></MPinHost>
    <!-- the page inside the control panel is a document of its own: its rows (a setting, a client, an application, a line of the log) get the window's menu here; what the panel owns (search, connect, restart) stays the panel's -->
    <MContextMenu ref="contextMenu" @select="onContextSelect" />
    <MContextProvider :locale="String(uiLocale)" :callbacks="embeddedCtxCallbacks" :capabilities="{ product: 'web', developer: developerMode.enabled.value }" />
    <WebDialogHost />
  </div>
  <div v-else class="m-app m-app--scroll m-web">
    <MPinHost ref="pinHost" storage-key="web-home" :locale="String(uiLocale)">
    <MAnnouncementPopup v-if="!hideList" :item="announcements.popup.value" :locale="String(uiLocale)" @shown="announcements.markShown" @acknowledge="announcements.acknowledge" @hide="announcements.hide" @open-url="(u) => openExternalUrl(u).catch(() => {})" />
    <MCoachMarks v-if="!hideList" :locale="String(uiLocale)" storage-key="foundation.coach.web" guide-key="foundation.guide.web" />
    <MCaption v-if="!hideList" bem="web-caption"><template v-if="backButton" #back><MCaptionBack :page="backPage" :locale="String(uiLocale)" @back="jumps.back()" /></template><template #version><MCaptionVersion :version="shellVersion" :build="buildInfo" :os="osName()" :locale="String(uiLocale)" :update="notice.version" @copied="onVersionCopied" @open-update="nav.go('about')" /></template><template #brand><MBrand class="web-caption__brand" :size="20" /></template><template #announcements><MStatusBell :locale="String(uiLocale)" /><MAnnouncementButton :unread="announcements.unread.value" :locale="String(uiLocale)" @open="nav.go('announcements')" /></template><template #search><div class="web-caption__search"><MUnifiedSearch :search="uni" :texts="searchT" :compact="compactSearch || caption.tight.value" :height="38" /></div></template><template v-if="compactSearch" #account><ShellUtils placement="bar" /></template><template #connect><button type="button" class="m-caption-connect" :aria-label="connectT.title" :data-tip="connectT.title" @click="connect.show('pair')"><MIcon name="person_add" /><span>{{ connectT.title }}</span></button></template></MCaption>
    <MScene v-if="!hideList" :nav="nav" :caption-height="48" :jumps="jumps" :chip="backChip" :rail-items="railItems" :texts="sceneTexts" :bar-items="barItems" :bar-current="barCurrent" :overlay="hostUnusable" :status="hostUnusable" storage-key="foundation-ui.web.list" @rail="onRail">
      <template #back-chip><MBackChip :page="backPage" :locale="String(uiLocale)" @back="jumps.back()" /></template>
      <template #rail-foot><ShellUtils placement="rail" /></template>
      <template #list="{ entry, scene }">
        <MNavList
          :groups="listGroups(entry.rail)"
          :current="currentRow"
          :label="railLabel(entry.rail)"
          :title="railLabel(entry.rail)"
          :titled="scene.kind === 'single'"
          @navigate="onNavigate"
          @context="onRowContext"
        />
      </template>
      <!-- the host cannot be used: ONE status page over the whole content area (the overlay), the page under it stays mounted and inert, so an unsaved edit is there when the host is back; a page that was not open then is not mounted against a host that is not there -->
      <template #detail="{ entry }"><component :is="VIEWS[entry.id]" v-if="!hostUnusable || entry.id === frozenId" :params="entry.params" /></template>
      <template #overlay><MHostOffline v-if="hostUnusable" :state="hostConn.state" :locale="String(uiLocale)" @retry="hostConn.retry" /></template>
      <template #supporting="{ entry }"><component :is="SUPPORTING[entry.id]" :params="entry.params" /></template>
    </MScene>
    <template v-else><div v-show="!hostUnusable" :inert="hostUnusable ? '' : undefined"><component :is="VIEWS[nav.current.value || 'home']" /></div><MHostOffline v-if="hostUnusable" :state="hostConn.state" :locale="String(uiLocale)" @retry="hostConn.retry" /></template>
    <MContextMenu ref="contextMenu" @select="onContextSelect" />
    <MContextProvider :locale="String(uiLocale)" :callbacks="ctxCallbacks" :capabilities="{ product: 'web', print: true, developer: developerMode.enabled.value }" />
    <DevReportHost v-if="!embedded" :locale="String(uiLocale)" />
    <MErrorHost v-if="!embedded" :locale="String(uiLocale)" report-sheet :can-go-back="nav.canPop.value" :host-online="hostConn.state.status === 'online'" :restart="restartHost" :open-url="(u) => openExternalUrl(u).catch(() => {})" @back="errorBack" />
    <ConfirmDialog
      :show="logoutAsk"
      dialog-id="shell-logout-confirm"
      :title="$t('troubleshooting.confirm_logout')"
      title-icon="logout"
      tone="danger"
      :close-label="$t('_common.close')"
      @close="logoutAsk = false"
    >
      <p>{{ $t('troubleshooting.confirm_logout_desc') }}</p>
      <template #actions>
        <MButton variant="text" @click="logoutAsk = false">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" icon="logout" @click="confirmLogout">{{ $t('troubleshooting.logout') }}</MButton>
      </template>
    </ConfirmDialog>
    <MConnectDialog v-model="connect.open.value" v-model:tab="connect.tab.value" v-model:os="connect.os.value" :assets-of="connect.assetsOf" :version-of="connect.versionOf" :locale="String(uiLocale)" :qr="connect.matrix" :pair-qr="connect.pairQr" :count="hostClients.length" :latest="hostClients.length ? hostClients[hostClients.length - 1].name : ''" :close-label="$t('_common.close')"
      @pair-pin="(e) => connect.pairPin(e.pin)" @pair-qr-begin="connect.qrBegin()" @pair-qr-stop="connect.qrStop()" @open-url="(u) => openExternalUrl(u).catch(() => {})" @open-downloads="() => { connect.open.value = false; jumpNav.go('downloads') }" />
    <MPairPrompt :requests="pairing.requests.value" :locale="String(uiLocale)" @submit="onPairSubmit" @reject="(id) => pairing.reject(id)" @install="connect.show('install')" />
    <PendingHosts />
    <WebDialogHost />
    </MPinHost>
  </div>
</template>

<script setup>
import { computed, defineAsyncComponent, defineComponent, getCurrentInstance, h, onBeforeUnmount, onMounted, provide, ref, shallowRef, watch, watchEffect } from 'vue'
import { useI18n } from 'vue-i18n'
import QRCode from 'qrcode'
import { trackEvents, setTelemetryContext } from '../config/telemetry.js'
import { answerRedactRequests, homeTexts, MContextProvider, useTelemetryConsent, MErrorHost, installErrorHandlers, setErrorContext, startAutoReport, MPageLoading, useNotify, useSystemNotices, MStatusBell, MPage, MGroup, MItem, MSection, MIcon, MBrand, MCaption, MCaptionVersion, MCaptionBack, MBackChip, useCaptionRoom, createJumpHistory, useBackVariant, showsButton, showsChip, captionTexts, MCoachMarks, MConnectDialog, MPairPrompt, MPinHost, pinSpecForPage, pinTexts, pinToHome, isPinnedOnHome, createPairingPending, MSnackbar, connectTexts, useConnectDialog, MUnifiedSearch, useUnifiedSearch, requestPageFilter, searchTexts, actionDocs, settingDocs, clientDocs, appDocs, tileDocs, addTileToHome, MHostOffline, useHostConnection, MButton, MContextMenu, MNavList, MScene, aboutTexts, latencyClockTexts, statusTexts, networkOverviewTexts, doctorTexts, testToolTexts, createNav, announcementTexts, useAnnouncements, MAnnouncementButton, MAnnouncementPage, MAnnouncementPopup, ackedAt, useAppearance, useSettingsList, useErrors, useDeveloperMode, DevReportHost, registerDevReport, openDevReport, devmodeTexts } from 'foundation-ui/vue'
import { afterCheck, afterOpened, noticeView, readNotice, writeNotice } from 'foundation-ui/update'
import { SEARCH_INDEX, categoryOfRow, labelOf, parseSettingsHash, rowIdOf, settingsHash } from 'foundation-ui/settings/index.js'
import { canOpenBigScreen, openBigScreenOnHost } from '../composables/openBigScreen.js'
import ShellUtils from '../components/layout/ShellUtils.vue'
import { streamTimerTexts } from './streamTimerTexts.js'
import PendingHosts from '../components/layout/PendingHosts.vue'
import WebDialogHost from '../components/layout/WebDialogHost.vue'
import { registerPending } from '../composables/pendingSources.js'
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import { useLogout } from '../composables/useLogout.js'
import { useShellLive } from '../composables/useShellLive.js'
import { useUpdateStatus } from '../composables/useUpdateStatus.js'
import { useComponentStatus } from '../composables/useComponentStatus.js'
import { SPONSORS } from '../config/sponsors.js'
import { openExternalUrl } from '../utils/helpers.js'
import { rowKeywords } from 'foundation-ui/settings/index.js'
import { hostData } from '../composables/hostData.js'
import { apiFetch, apiJson } from '../utils/apiFetch.js'
import { lazyPane } from '../utils/lazyPane.js'
import { createFramedBack } from '../utils/framedBack.js'

const { t, te, locale: uiLocale } = useI18n()
// While the page is open the server can stop answering: the content area then shows the offline state with the countdown (a dead server cannot be restarted from
// here, so no restart); the page is as it was when the server is back, the pages load their data again.
const hostConn = useHostConnection({
  // A response, including the host's temporary 503, proves that this HTTP
  // listener answered. Fetch rejects for a transport failure; the shared
  // connection state then requires consecutive failed probes before offline.
  // [web-rfc9110-20261109 §15.6.4; web-whatwg-fetch-20261109 §2.2.6]
  probe: async () => { const response = await fetch('/', { cache: 'no-store', signal: AbortSignal.timeout(2500) }); return response.status >= 100 && response.status < 600 },
  onRecovered: () => window.dispatchEvent(new Event('focus')),
})
// The host cannot be used (it is down, or it runs and does not answer): the content area is one status page. The page that was open stays mounted under it (inert), the others are not mounted against a host that is not there.
const hostUnusable = computed(() => hostConn.state.status !== 'online')
const frozenId = ref('')
watch(hostUnusable, (off) => { frozenId.value = off ? String(nav.current.value || '') : '' })

// One back stack for the whole Web UI. A rail item is a category; its list holds the pages (detail entries).
// A page that is not loaded yet shows the one loading state in the content area (after a short delay), never a blank.
const LoadingView = defineComponent({ name: 'PageLoadingView', setup: () => () => h('div', { class: 'm-page' }, [h('div', { class: 'm-page__column', style: 'padding-top: 80px' }, [h(MPageLoading)])]) })
const lazy = (loader) => lazyPane(loader, { loading: LoadingView })
const VIEWS = {
  home: lazy(() => import('./Home.vue')),
  pin: lazy(() => import('./Pin.vue')),
  apps: lazy(() => import('./Apps.vue')),
  config: lazy(() => import('./Config.vue')),
  downloads: lazy(() => import('./Downloads.vue')),
  troubleshooting: lazy(() => import('./Troubleshooting.vue')),
  password: lazy(() => import('./SecurityLogin.vue')),
  about: lazy(() => import('./About.vue')),
  appearance: lazy(() => import('./Appearance.vue')),
  'latency-clock': lazy(() => import('./LatencyClock.vue')),
  'test-tools': lazy(() => import('./TestTools.vue')),
  'stream-timer': lazy(() => import('./StreamTimer.vue')),
  components: lazy(() => import('./ComponentStatus.vue')),
  doctor: lazy(() => import('./Doctor.vue')),
  network: lazy(() => import('./NetworkOverview.vue')),
}
// the destinations of 输入与外设 and 显示 that are the configuration opened at a place (the panel has a page for each: here it is the same page of the settings)
const PLACES = {
  gamepad: { tab: 'input', sub: 'gamepad' },
  keyboard: { tab: 'input', sub: 'keyboard' },
  mouse: { tab: 'input', sub: 'mouse' },
  usb: { tab: 'input', sub: 'usb' },
  microphone: { tab: 'audio', sub: 'microphone' },
  'audio-output': { tab: 'audio', sub: 'sound' },
  'display-output': { tab: 'display', sub: 'output' },
  'display-modes': { tab: 'display', sub: 'modes' },
  'display-hdr': { tab: 'display', sub: 'hdr' },
  'display-encoders': { tab: 'display', sub: 'encoders' },
  vdd: { tab: 'display', sub: 'virtual' },
  'display-advanced': { tab: 'display', sub: 'advanced' },
}
// a place is its settings page at the place; a row to jump to (a search hit, a link) comes with the entry
for (const [id, params] of Object.entries(PLACES)) VIEWS[id] = defineComponent({ name: `Place_${id}`, inheritAttrs: false, props: { params: { type: Object, default: null } }, setup: (props) => () => h(VIEWS.config, { params: { ...params, ...(props.params?.sub && props.params.tab === params.tab ? { sub: props.params.sub } : {}), ...(props.params?.focus ? { focus: props.params.focus } : {}) } }) })
// The one 设置 entry of the bottom bar of a narrow window: a list of the four settings categories, each opening its own list (as in the control panel)
const SETTINGS_CATEGORIES = ['config', 'input', 'audio', 'display', 'system']
// the second line of each category row (what is inside), set from the shared row lines below
const hubLines = shallowRef({})
// 公告: every announcement, opened from the bell of the caption bar
VIEWS.announcements = defineComponent({ name: 'AnnouncementsPage', inheritAttrs: false, props: { params: { type: Object, default: null } }, setup: (props) => () => h(MAnnouncementPage, { openId: props.params?.id || '', onRead: announcements.read, items: announcements.list.value, locale: String(uiLocale.value), ackedAt, onRefresh: () => announcements.refresh(true), onDismiss: announcements.dismiss, onOpenUrl: (u) => openExternalUrl(u).catch(() => {}), onBack: () => nav.pop() }) })
// 设置 of the narrow window: every page of the five categories at once, in sections named by the category, so a page is two presses away (the bar entry, the page)
const hubPages = (id) => (id === 'config'
  ? settingsList.groups.value.flatMap((g) => g.items)
  : Object.entries(PAGES).filter(([, p]) => p.rail === id && !p.hub).filter(onThisHost).map(([pid, p]) => ({ id: pid, icon: p.icon, label: pageLabel(p), text: settingsList.lines.value[ROW_OF_PAGE[pid]] })))
VIEWS['settings-hub'] = defineComponent({
  name: 'SettingsHub',
  inheritAttrs: false,
  setup: () => () => h(MPage, { title: t('shell.settings') }, { default: () => SETTINGS_CATEGORIES.map((id) => h(MSection, { key: id, title: t(RAIL.find((x) => x.id === id).label) }, { default: () => h(MGroup, null, { default: () => hubPages(id).map((item) => h(MItem, { key: item.id, action: true, icon: item.icon, title: item.label, text: item.text, onClick: () => (id === 'config' ? onNavigate(item) : nav.go(item.id)) })) }) })) }),
})
// 串流 of the narrow window: the same page as 设置 (title, one group of rows with plain icons, the wallpaper behind); it only lists pages, 添加应用 is the button of the page 串流应用
VIEWS['stream-hub'] = defineComponent({
  name: 'StreamHub',
  inheritAttrs: false,
  setup: () => () => h(MPage, { title: t('shell.stream') }, {
    default: () => h(MSection, null, { default: () => h(MGroup, null, { default: () => hubPages('stream').map((item) => h(MItem, { key: item.id, action: true, icon: item.icon, title: item.label, text: item.text, onClick: () => nav.go(item.id) })) }) }),
  }),
})
// Pages that show a supporting pane next to them (windows from 1200 px) name it in their route
const SUPPORTING = {}
// rail: category of the page; path: URL; icon/tone/label: how the list shows it
const PAGES = {
  home: { rail: 'overview', path: '/', list: false },
  pin: { rail: 'stream', path: '/pin', icon: 'devices', tone: 'tertiary', label: 'navbar.clients' },
  apps: { rail: 'stream', path: '/apps', icon: 'apps', tone: 'secondary', label: 'navbar.applications' },
  downloads: { rail: 'stream', path: '/downloads', icon: 'download', tone: 'success', label: 'resource_card.client_downloads' },
  config: { rail: 'config', path: '/config', icon: 'tune', tone: 'primary', label: 'navbar.configuration' },
  password: { rail: 'config', path: '/password', icon: 'verified_user', tone: 'error', label: 'navbar.security' },
  // 输入与外设: a place of the input tab each (USB forwarding exists on a Windows host only; the list of the devices on the computer is the control panel's own page)
  gamepad: { rail: 'input', place: true, path: '/config', icon: 'sports_esports', tone: 'primary', label: 'shell.gamepad' },
  keyboard: { rail: 'input', place: true, path: '/config', icon: 'keyboard', tone: 'secondary', label: 'config.input_keyboard_title' },
  mouse: { rail: 'input', place: true, path: '/config', icon: 'mouse', tone: 'tertiary', label: 'config.input_mouse_title' },
  usb: { rail: 'input', place: true, windows: true, path: '/config', icon: 'usb', tone: 'success', label: 'shell.usb_forwarding' },
  // 音频: a place of the audio tab each
  'audio-output': { rail: 'audio', place: true, path: '/config', icon: 'speaker', tone: 'primary', label: 'shell.audio_output' },
  microphone: { rail: 'audio', place: true, path: '/config', icon: 'mic', tone: 'tertiary', label: 'shell.microphone' },
  // 显示与视频: a place of the display tab each (HDR and the virtual display exist on a Windows host only)
  // (the tree of displayTree.js; 画质增强 is the control panel's page; 高级 is the last row and stands apart)
  'display-output': { rail: 'display', place: true, path: '/config', icon: 'monitor', tone: 'primary', label: 'shell.display_output' },
  'display-modes': { rail: 'display', place: true, path: '/config', icon: 'aspect_ratio', tone: 'secondary', label: 'shell.display_modes' },
  'display-hdr': { rail: 'display', place: true, windows: true, path: '/config', icon: 'hdr_on', tone: 'tertiary', label: 'shell.display_hdr' },
  'display-encoders': { rail: 'display', place: true, path: '/config', icon: 'memory', tone: 'secondary', label: 'shell.display_encoders' },
  vdd: { rail: 'display', place: true, windows: true, path: '/config', icon: 'desktop_windows', tone: 'success', label: 'shell.virtual_display' },
  'display-advanced': { rail: 'display', place: true, apart: true, path: '/config', icon: 'tune', tone: 'primary', label: 'shell.display_advanced' },
  // no page of its own on the host: the address is the current page with a hash (#about, #components), so it can be linked and a refresh keeps the place
  components: { rail: 'system', path: null, hash: 'components', icon: 'monitor_heart', tone: 'success', text: (locale) => statusTexts(locale).title },
  network: { rail: 'system', path: null, hash: 'network-overview', icon: 'lan', tone: 'primary', text: (locale) => networkOverviewTexts(locale).title },
  troubleshooting: { rail: 'system', path: '/troubleshooting', icon: 'stethoscope', tone: 'warn', label: 'shell.logs_diagnostics' },
  doctor: { rail: 'system', path: null, hash: 'doctor', icon: 'health_and_safety', tone: 'success', text: (locale) => doctorTexts(locale).title },
  'settings-hub': { rail: 'config', list: false, path: null, hash: 'settings' },
  // 串流 of a narrow window: the pages of the stream rail as one page of grouped rows, like 设置 (the rail's list page does not exist on a phone)
  'stream-hub': { rail: 'stream', list: false, hub: true, path: null, hash: 'streaming' },
  announcements: { rail: 'overview', list: false, push: true, path: null, hash: 'announcements', icon: 'notifications', tone: 'primary', text: (locale) => announcementTexts(locale).title },
  'test-tools': { rail: 'system', path: null, hash: 'test-tools', icon: 'science', tone: 'secondary', text: (locale) => testToolTexts(locale).title },
  'latency-clock': { rail: 'system', path: null, hash: 'latency-clock', icon: 'speed', tone: 'tertiary', text: (locale) => latencyClockTexts(locale).title },
  'stream-timer': { rail: 'system', path: null, hash: 'stream-timer', icon: 'timer', tone: 'tertiary', text: (locale) => streamTimerTexts(locale).title },
  appearance: { rail: 'appearance', list: false, path: null, hash: 'appearance', icon: 'palette', tone: 'primary', text: (locale) => labelOf(locale, 'shell.appearance') },
  about: { rail: 'help', rootRail: 'about', list: false, push: true, path: null, hash: 'about', icon: 'info', tone: 'primary', text: (locale) => aboutTexts(locale).title },
}
// the rail of the control panel, without what only the control panel can do (the local USB/IP, the picture enhancement, the toolbox, 米塔 AI, the panel's own settings): see docs/parity.md of foundation-ui
const RAIL = [
  { id: 'overview', icon: 'home', label: 'navbar.home' },
  { id: 'stream', icon: 'cast_connected', label: 'shell.stream' },
  { id: 'config', icon: 'tune', label: 'shell.stream_settings' },
  { id: 'input', icon: 'sports_esports', label: 'shell.input_peripherals' },
  { id: 'audio', icon: 'volume_up', label: 'shell.audio' },
  { id: 'display', icon: 'desktop_windows', label: 'shell.display' },
  { id: 'system', icon: 'build', label: 'shell.system' },
  { id: 'help', icon: 'help', label: 'shell.help_about' },
]
const routes = {}
for (const r of RAIL) if (r.id !== 'overview' && r.id !== 'help') routes[`${r.id}:list`] = { role: 'list', rail: r.id }
for (const [id, p] of Object.entries(PAGES)) routes[id] = { role: 'detail', rail: p.rail, list: p.list, supporting: p.supporting, push: p.push, rootRail: p.rootRail }

const normalise = (path) => path.replace(/\.html$/, '').replace(/\/+$/, '') || '/'
const pageLabel = (p) => (p.text ? p.text(uiLocale.value) : t(p.label))
const idFromPath = (path) => {
  const p = normalise(path)
  if (p === '/index') return 'home'
  if (p === '/diagnostics') return 'troubleshooting'
  return Object.keys(PAGES).find((id) => PAGES[id].path === p) || 'home'
}
// the pages that live behind a hash
const HASH_PAGES = Object.fromEntries(Object.entries(PAGES).filter(([, p]) => p.hash).map(([id, p]) => [p.hash, id]))
// a page inside the gamepad place (per game, a DualSense group) is the gamepad place at that page
const placeOf = (params) => Object.keys(PLACES).find((id) => Object.entries(PLACES[id]).every(([k, v]) => params?.[k] === v) && !(params?.sub && !PLACES[id].sub)) || (params?.tab === 'input' && params.sub ? 'gamepad' : undefined)
const hashPage = (hash) => HASH_PAGES[String(hash || '').replace(/^#/, '')]
// a phone has no list pages of the rails: 串流 is the stream hub, every other category is in the 设置 hub; Back from a page is that hub, whatever way the page was reached
const HUB_OF = { stream: 'stream-hub', config: 'settings-hub', input: 'settings-hub', audio: 'settings-hub', display: 'settings-hub', system: 'settings-hub' }
const isPhone = () => window.innerWidth < 600
const nav = createNav({
  routes,
  home: 'home',
  compact: isPhone,
  hubOf: HUB_OF,
  // in a frame (the control panel's) the panel owns Back: an entry added here to the window's history would be the first thing its mouse back button undoes
  mirror: window.parent !== window ? 'replace' : 'push',
  entriesFrom: (location) => {
    // the old address of the DualSense page: it is part of the gamepad page of 输入 now
    if (normalise(location.pathname) === '/dualsense') return [{ id: 'gamepad' }]
    const id = hashPage(location.hash) || idFromPath(location.pathname)
    // the configuration opens on the tab (and setting) that the address names
    if (id === 'config') {
      const params = parseSettingsHash(location.hash, SEARCH_INDEX) || { tab: 'general' }
      const place = placeOf(params)
      return [place ? { id: place } : { id, params }]
    }
    return [{ id }]
  },
  urlFor: (entries) => {
    const detail = entries.slice().reverse().find((e) => e.role === 'detail')
    const target = PAGES[detail ? detail.id : 'home'].path
    const page = PAGES[detail ? detail.id : 'home']
    const here = normalise(location.pathname) + (hashPage(location.hash) ? location.hash : '')
    const want = page.hash ? `${normalise(location.pathname)}#${page.hash}` : page.place ? `/config${settingsHash(PLACES[detail.id])}` : target
    return !want || here === want ? undefined : want
  },
})

// Inside the control panel the panel's own navigation is the shell; the page shows only its content.
const embedded = window.isTauri === true && window.parent !== window
registerPending({ t, te, locale: uiLocale.value, embedded })
const embeddedId = ref(idFromPath(window.location.pathname))
// The control panel keeps ONE frame for the pages of the Web (a new frame per address was a new document: html, scripts, mount, first paint, 136 to 151 ms on the real host). It tells the frame the page by a message and the frame
// swaps the view; the address is replaced (never pushed: Back is the panel's) so that a reload of the frame lands on the same page. At idle the chunks of the other framed pages are loaded, so the swap finds them in memory.
const FRAMED_IDS = ['pin', 'apps', 'config', 'downloads', 'password']
if (embedded) {
  window.addEventListener('message', (event) => {
    if (event.source !== window.parent || event.data?.type !== 'foundation-frame-go' || typeof event.data.path !== 'string') return
    const path = event.data.path
    if (!path.startsWith('/') || path.startsWith('//')) return
    const id = idFromPath(path.split(/[?#]/, 1)[0])
    if (!FRAMED_IDS.includes(id)) return
    try { history.replaceState(history.state, '', path) } catch { /* a frame that cannot rewrite its address keeps it */ }
    embeddedId.value = id
  })
  onMounted(() => window.parent.postMessage({ type: 'foundation-frame-go-ready' }, '*'))
  // Back in the title row of the page itself (MPage's arrow): the page injects this as its scene; the panel says when Back applies and gets the press (utils/framedBack.js)
  const framedBack = createFramedBack({ post: (message) => window.parent.postMessage(message, '*'), parent: window.parent })
  provide('m-scene', framedBack.scene)
  window.addEventListener('message', framedBack.onMessage)
  onMounted(framedBack.start)
  const warm = () => { for (const id of FRAMED_IDS) VIEWS[id]?.__asyncLoader?.().catch(() => {}) }
  ;(window.requestIdleCallback || ((fn) => setTimeout(fn, 1500)))(warm, { timeout: 4000 })
}
if (embedded) document.documentElement.dataset.sunshineGui = 'true'
// the control panel's own search bar is the one search: the keys that open it are handed to the panel from inside the frame
if (embedded) {
  window.addEventListener('keydown', (e) => {
    const field = /^(INPUT|TEXTAREA|SELECT)$/.test(e.target?.tagName) || e.target?.isContentEditable
    if (((e.ctrlKey || e.metaKey) && !e.altKey && /^[kf]$/i.test(e.key)) || (e.key === '/' && !field && !e.ctrlKey && !e.metaKey && !e.altKey)) { e.preventDefault(); window.parent.postMessage({ type: 'foundation-search-open' }, '*') }
  }, true)
}
document.documentElement.classList.toggle('m-web-shell', !embedded)

const hideList = ref(false)
// Every uncaught error of the shell is drawn in our own surface (MErrorHost): the way back first, then the restart of Sunshine, then copy, download and the report to the developers.
// The choice about usage counts and error information (the first answer is asked once, when nothing else is on screen; 关于 changes it later)
const telemetry = useTelemetryConsent({ api: { getJson: (path) => apiJson(path), postJson: (path, body) => apiJson(path, { method: 'POST', body }) } })
provide('telemetryConsent', telemetry)
// one page view per page, from here; the version, system and channel split the numbers
watch(() => nav.current.value, (page) => { if (!embedded && page) trackEvents.pageView(String(page)) }, { immediate: true })
const osName = () => { const n = typeof navigator === 'undefined' ? '' : navigator; return String(n.userAgentData?.platform || n.platform || '') }
const errorBack = () => { if (nav.canPop.value) nav.pop(); else nav.selectRail('overview') }
const restartHost = async () => { await apiFetch('/api/restart', { method: 'POST' }).catch(() => null) }
const errorContext = () => ({ product: 'web', version: shellVersion.value, os: osName(), page: String(nav.current.value || ''), locale: String(uiLocale.value), logs: String(recentLogText.value || '').split('\n').slice(-40).join('\n'), components: `webui=${shellVersion.value || '?'}` })
setErrorContext(errorContext)
// a browser cannot take a screenshot of its page or read the host log: the report sheet says so; the report goes privately to the developers or into a zip, never to a public issue
const developerMode = useDeveloperMode()
registerDevReport({
  // the host log's last lines (the host answers it to a page of its own origin); the lines the error report carried are added by the sheet
  logs: async () => { await refreshLogs(); return String(recentLogText.value || '') },
  env: () => [`product: Foundation Sunshine Web UI`, `version: ${shellVersion.value || ''}`, `os: ${osName()}`, `locale: ${String(uiLocale.value)}`, `user agent: ${navigator.userAgent}`].join('\n'),
  save: (name, bytes, mime) => { const link = document.createElement('a'); link.href = URL.createObjectURL(new Blob([bytes], { type: mime })); link.download = name; document.body.appendChild(link); link.click(); link.remove(); setTimeout(() => URL.revokeObjectURL(link.href), 4000) },
  product: 'web',
})
let removeErrorHandlers = null
const recentLogText = ref('')
const refreshLogs = async () => { try { const r = await apiFetch('/api/logs'); if (r.ok) recentLogText.value = (await r.text()).split('\n').slice(-60).join('\n') } catch { /* a report without log lines */ } }
const shellApp = getCurrentInstance()?.appContext.app
// in the Panel's frame: the page around it asks for the marked boxes before it takes a screenshot (rectangles only)
onMounted(async () => { if (embedded) { answerRedactRequests(); return }; removeErrorHandlers = installErrorHandlers({ app: shellApp }); startAutoReport({ context: errorContext }); void refreshLogs(); await telemetry.refresh() })
onBeforeUnmount(() => removeErrorHandlers?.())
// A page action that the list pane's button asks for ('pair', 'addApp'); the page takes it and clears it.
const pending = ref('')
const logoutAsk = ref(false)
const { logout } = useLogout()
const confirmLogout = () => { logoutAsk.value = false; logout({ onLocalhost: () => { window.location.href = '/' } }) }
// One context menu for the window; a page that opens it passes the handler of its own entries.
const contextMenu = ref(null)
// the window's own context menu (SPEC-context-menu): what this product can do for it; an action that is not here is not an item
// 运行体检 is 全部检查 of the components page: the page opens and every check runs
const ctxCallbacks = { search: (text) => { uni.query.value = text; uni.show() }, connect: () => { connect.open.value = true }, notice: (m) => useNotify().snack(m), openExternal: (u) => openExternalUrl(u), restartHost: () => restartHost(), doctor: () => { componentStatus.openPage(); void componentStatus.checkAll() },
  // developer mode (seven taps on the version): 「在 Issue 中报告此处」 opens the report with the place filled in; the browser has no DevTools button to press, the menu says which key
  reportHere: ({ route, place, text }) => { void openDevReport({ kind: 'feedback', draft: { title: '', description: `${devmodeTexts(String(uiLocale.value)).ctxReport}: ${route}${place ? ` (${place})` : ''}${text ? ` — ${text}` : ''}\n\n` } }) } }
const embeddedCtxCallbacks = { notice: (m) => useNotify().snack(m), openExternal: (u) => openExternalUrl(u) }
const pinHost = ref(null)
let menuHandler = null
const showMenu = (event, items, context, handler) => { menuHandler = handler || null; contextMenu.value.show(event, items, context) }
// the one-time thank-you line after the guided first run
// the configuration opened at a place of the settings: the destinations of 输入与外设 and 显示 are those places
const openConfig = (params) => { const place = placeOf(params); if (place) nav.go(place, params?.focus ? { focus: params.focus } : undefined); else nav.go('config', params) }
// ---- the global back: the places a person left by a jump. The pages (体检, 关于, the home tiles, 网络概览) move through `webShell.nav` and `webShell.openConfig`, which are jumps; the rail, the list and the
// search move through the shell's own `nav`, which is the person's move and clears the stack (see foundation-ui createJumpHistory)
const jumps = createJumpHistory({
  locate: () => ({ id: nav.current.value, params: nav.detail.value?.params, rail: nav.rail.value }),
  restore: (place) => { if (place.params?.tab || place.id === 'config') openConfig(place.params || { tab: 'general' }); else nav.go(place.id, place.params) },
})
jumps.attach(nav.entries)
const jumpNav = { ...nav, go: (id, params, opts) => jumps.jump(() => nav.go(id, params, opts)) }
const jumpConfig = (params) => jumps.jump(() => openConfig(params))
const { variant } = useBackVariant()
const backButton = computed(() => showsButton(variant.value))
const backChip = computed(() => showsChip(variant.value) && jumps.canBack.value)
const backPage = computed(() => {
  const place = jumps.top.value
  if (!place) return ''
  if (place.params?.tab) return labelOf(String(uiLocale.value), `tabs.${place.params.tab}`) || railLabel(place.rail)
  const page = PAGES[place.id]
  return (page ? pageLabel(page) : '') || railLabel(place.rail)
})
provide('webShell', { hideList, nav: jumpNav, openConfig: jumpConfig, pending, showMenu, openSetting: (key) => { const entry = settingsList.searchItems.value.find((e) => e.keyName === key); if (entry) onNavigate(entry) }, askLogout: () => { logoutAsk.value = true } })

// Second lines of the list rows from live host data
const live = useShellLive()
watch(() => nav.rail.value, () => live.refresh())

const shellVersion = ref('')
// the version goes with every batch (the host adds the system, the channel and the GPU vendor); an embedded page counts nothing, the control panel around it does
watch(shellVersion, (v) => { if (v) { setTelemetryContext({ version: v, embedded }); if (!embedded) trackEvents.appStart() } })
onMounted(async () => { try { shellVersion.value = (await hostData.refresh('config'))?.version || '' } catch { /* the version only narrows version-bound notices */ } })
const announcements = useAnnouncements({ locale: uiLocale, version: shellVersion })
provide('announcementsRefresh', () => announcements.refresh(true))
const announcementsUnread = announcements.unread
// the update check of the about page runs quietly once; a dot on the help rail says there is something to install
const update = useUpdateStatus()
// the version in the caption bar and the notice of a waiting update: the one quiet check of this page (useUpdateStatus) is the only source, nothing is asked again here
const buildInfo = ref(null)
onMounted(async () => { try { const v = await apiJson('/api/version'); buildInfo.value = v && typeof v === 'object' && v.commit ? v : null } catch { buildInfo.value = null } })
const noticeState = ref(readNotice(localStorage))
const remember = (next) => { noticeState.value = next; writeNotice(localStorage, next) }
watch([() => update.state.value, () => update.nextVersion.value], ([state, next]) => {
  if (state === 'available' && next) remember(afterCheck(noticeState.value, { version: next }, { installed: shellVersion.value }))
  else if (state === 'latest') remember(afterCheck(noticeState.value, { is_latest: true }))
}, { immediate: true })
const notice = computed(() => (shellVersion.value ? noticeView(noticeState.value, { installed: shellVersion.value }) : { waiting: false, version: '', dot: false }))
// 软件更新 is in 关于: opening it puts the dot out for the version that waits; a newer one brings it back
watch([() => nav.current.value, () => notice.value.version], ([page]) => { if (page === 'about' && noticeState.value.available) remember(afterOpened(noticeState.value)) }, { immediate: true })
const onVersionCopied = (text) => useNotify().snack(text ? captionTexts(String(uiLocale.value)).copiedNote : captionTexts(String(uiLocale.value)).copyFailed)
// the component status of the host: the page, and the cards the home catalogue offers, read this one controller
const componentStatus = useComponentStatus({ locale: uiLocale, openPage: () => jumpNav.go('components'), openAbout: () => jumpNav.go('about') })
// what is wrong with the host or the app right now goes to the status button of the caption bar
useSystemNotices({
  status: componentStatus,
  locale: uiLocale,
  go: { components: () => jumpNav.go('components'), microphone: () => jumpNav.go('microphone'), vdd: () => jumpNav.go('vdd'), update: () => jumpNav.go('about') },
  update: () => ({ state: update.available.value ? 'available' : '', next: '' }),
})
const railItems = computed(() => RAIL.map((r) => ({ ...r, label: t(r.label), ...(r.id === 'overview' && announcementsUnread.value ? { badge: announcementsUnread.value } : {}), ...(r.id === 'help' && notice.value.dot ? { badge: 'dot' } : {}) })))
// the bottom bar: four destinations with their full labels, the four settings categories behind one
const BAR_GROUP = { overview: 'overview', stream: 'stream', config: 'settings-hub', input: 'settings-hub', audio: 'settings-hub', display: 'settings-hub', system: 'settings-hub', help: 'help' }
const barItems = computed(() => {
  const by = (id) => railItems.value.find((r) => r.id === id)
  return [by('overview'), by('stream'), { id: 'settings-hub', icon: 'tune', label: t('shell.settings') }, by('help')]
})
const barCurrent = computed(() => (nav.current.value === 'settings-hub' ? 'settings-hub' : BAR_GROUP[nav.rail.value] || 'overview'))
const railLabel = (rail) => t(RAIL.find((r) => r.id === rail)?.label || 'navbar.home')
// The settings list is the shared model (categories in groups, one live line each, recently used); every other rail is one group of pages
const settingsList = useSettingsList({ locale: uiLocale, config: live.config, dualsenseKeys: live.dualsenseKeys, sources: { clients: computed(() => live.clients.value?.list), apps: live.apps, components: computed(() => componentStatus.rows.value), ds: live.ds, mic: live.mic } })
watchEffect(() => { hubLines.value = settingsList.lines.value })
// the row ids of foundation-ui's rowLines for the pages of the list panes
const ROW_OF_PAGE = { pin: 'clients', apps: 'apps', downloads: 'downloads', gamepad: 'gamepad', keyboard: 'keyboard', mouse: 'mouse', usb: 'usb', microphone: 'microphone', 'display-output': 'display-output', 'display-modes': 'display-modes', 'display-encoders': 'display-encoders', 'display-hdr': 'display-hdr', 'display-advanced': 'display-advanced', 'audio-output': 'audio-output', vdd: 'vdd', components: 'components', network: 'network', 'latency-clock': 'latency-clock', 'test-tools': 'test-tools', 'stream-timer': 'stream-timer', troubleshooting: 'diagnostics', about: 'help' }
// the pages of a host: the Windows-only ones (HDR, the virtual display) are not offered on another system
const hostPlatform = computed(() => live.config.value?.platform || 'windows')
const onThisHost = ([, p]) => !p.windows || hostPlatform.value === 'windows'
const listGroups = (rail) => (rail === 'config'
  ? settingsList.groups.value
  : railRows(rail))
// the rows of a rail; a page marked `apart` (the 高级 of 显示与视频) is a group of its own at the end of the list
const railRows = (rail) => {
  const rows = Object.entries(PAGES).filter(([, p]) => p.rail === rail && !p.hub).filter(onThisHost).map(([id, p]) => ({ id, apart: !!p.apart, icon: p.icon, tone: p.tone, label: pageLabel(p), text: id === 'about' && update.available.value ? aboutTexts(uiLocale.value).available : settingsList.lines.value[ROW_OF_PAGE[id]] }))
  const apart = rows.filter((r) => r.apart)
  return apart.length ? [{ id: rail, items: rows.filter((r) => !r.apart) }, { id: `${rail}-apart`, items: apart }] : [{ id: rail, items: rows }]
}
// The row that is highlighted: the page, or the category of the settings that is open
const currentRow = computed(() => {
  const category = categoryOfRow(nav.current.value, nav.detail.value?.params)
  return category ? rowIdOf(category) : nav.current.value
})

// Right click on a row: open it, its main action where it has one, copy the address
const ROW_ACTIONS = { pin: { id: 'pair', icon: 'person_add', label: 'connections.add' }, apps: { id: 'addApp', icon: 'add', label: 'apps.add_new' } }
// the rows that have a tile on the overview that opens them: 固定到概览 pins that tile (the pin menu of the rows of a setting, the same host)
const ROW_TILES = { pin: 'clients', apps: 'apps', downloads: 'downloads', troubleshooting: 'diagnostics' }
const rowPinSpec = (item) => (ROW_TILES[item.id] ? pinSpecForPage(ROW_TILES[item.id], { title: item.label, icon: item.icon }) : null)
function onRowContext(event, item) {
  const action = ROW_ACTIONS[item.id]
  const spec = rowPinSpec(item)
  const pinWords = pinTexts(String(uiLocale.value))
  showMenu(event, [
    { id: 'open', icon: 'open_in_new', label: t('shell.open') },
    ...(action ? [{ id: action.id, icon: action.icon, label: t(action.label) }] : []),
    ...(spec && pinHost.value ? [{ id: 'pin', icon: 'keep', label: pinHost.value.isPinned(spec) ? pinWords.unpin : pinWords.pin }] : []),
    { divider: true, id: 'd' },
    { id: 'copy', icon: 'content_copy', label: t('shell.copy_link') },
  ], item, null)
}
async function onContextSelect(entry, item) {
  if (menuHandler) { menuHandler(entry, item); return }
  if (entry.id === 'open') nav.go(item.route || item.id, item.params)
  else if (entry.id === 'pin') pinHost.value.toggle(rowPinSpec(item))
  else if (entry.id === 'copy') { try { await navigator.clipboard.writeText(new URL(PAGES[item.route || item.id].path, location.origin).href) } catch { /* clipboard unavailable */ } }
  else { nav.go(item.route || item.id, item.params); pending.value = entry.id }
}
const sceneTexts = computed(() => ({ resize: t('shell.resize'), main: t('navbar.home'), expand: t('shell.expand_nav'), collapse: t('shell.collapse_nav'), shortcuts: t('shell.shortcuts'), back: t('shell.back'), search: t('shell.search'), help: t('shell.shortcut_help'), section: t('shell.section'), close: t('shell.clear') }))
const onRail = (item) => (item.id === 'settings-hub' ? nav.go('settings-hub') : nav.selectRail(item.id))
// a hit of the search on a setting of the display or audio pages opens that place (its rail, its row), not the stream settings
// the path of a setting in the search starts with 主机设置 only where the setting is there: the display and audio pages are rails of their own
const settingsPlace = (e) => (e.params?.tab === 'display' || e.params?.tab === 'audio' || e.params?.tab === 'input' ? '' : railLabel('config'))
const onNavigate = (item) => (item.route === 'config' ? openConfig(item.params) : nav.go(item.route || item.id, item.params))
// ---- the unified search: one bar over everything a person can look for (an icon and a full-screen view on a narrow window) ----
const compactSearch = ref(window.innerWidth < 600)
// the version sits beside the name: when what is left of the bar cannot show the search field with its hint, the search is its icon (as on a phone) instead of being cut
const caption = useCaptionRoom()
let wasPhone = isPhone()
const onSearchResize = () => { compactSearch.value = window.innerWidth < 600; const now = isPhone(); if (now !== wasPhone) { wasPhone = now; if (now) nav.narrow(); else nav.widen() } }
const hostClients = ref([])
const hostApps = ref([])
async function loadSearchData() {
  try { const r = await apiFetch('/api/clients/list'); if (r.ok) hostClients.value = (await r.json()).named_certs || [] } catch { /* the search then has no clients */ }
  try { const r = await apiFetch('/api/apps'); if (r.ok) hostApps.value = (await r.json()).apps || [] } catch { /* the search then has no applications */ }
}
const searchT = computed(() => searchTexts(String(uiLocale.value)))
// 连接设备: the dialog of the caption button and of the search, over the host's own API
const connectT = computed(() => connectTexts(String(uiLocale.value)))
const connectNotice = ref('')
// short results go to the one snackbar of the window
watch(connectNotice, (message) => { if (message) { useNotify().snack(message); connectNotice.value = '' } })
const jsonOf = async (path, init) => (await apiFetch(path, init)).json()
const connect = useConnectDialog({
  hostApi: { getJson: (path) => jsonOf(path), postJson: async (path, body) => { const r = await apiFetch(path, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }); if (path === '/api/pin' && !r.ok) throw failing(r); return r.json() } },
  render: (url) => QRCode.toDataURL(url, { width: 280, margin: 2, color: { dark: '#000000', light: '#ffffff' } }),
  matrix: (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules,
  say: (kind, reason) => {
    const T = connectT.value
    connectNotice.value = kind === 'sent' ? T.pinSent : ({ incorrect: T.pinIncorrect, timeout: T.pinTimeout, network: T.pinNetwork, 'not-ready': T.pinNotReady }[reason] || T.pinFailed)
  },
  defaultName: String(uiLocale.value).startsWith('zh') ? '新客户端' : 'New client',
})
// a device waits for its PIN: the prompt comes forward on any page (the host's long poll, docs/api/pairing-pending.md); a host without the API leaves it off
const failing = (r) => Object.assign(new Error(`HTTP ${r.status}`), { status: r.status })
const pairingApi = {
  getJson: async (path) => { const r = await apiFetch(path); if (!r.ok) throw failing(r); return r.json() },
  postJson: async (path, body) => { const r = await apiFetch(path, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }); if (!r.ok) throw failing(r); return r.json() },
  delete: async (path) => { const r = await apiFetch(path, { method: 'DELETE' }); if (!r.ok) throw failing(r); return r.json() },
}
const onPairingArrive = (list) => { if (document.hidden && typeof Notification !== 'undefined' && Notification.permission === 'granted') for (const r of list) new Notification(connectTexts(String(uiLocale.value)).pendingTitle.replace('{name}', r.device_name)) }
const pairing = createPairingPending({ hostApi: pairingApi, onArrive: onPairingArrive })
const onPairSubmit = async (e) => { const ok = await pairing.submit(e.id, e.pin, e.name); connectNotice.value = ok ? connectT.value.pinSent : connectT.value.pinFailed }
onMounted(() => { if (!embedded) pairing.start() })
onBeforeUnmount(() => pairing.stop())
// a page inside the control panel keeps no home of its own: the pin is sent to the panel's shell
const forwardPin = (msg) => window.parent.postMessage({ type: 'm3e-pin', ...msg }, '*')
// The actions the search runs. The ones that stop something ask first (tier 3 of ACTION_DEFS); each one is what the button of the
// Troubleshooting page does, so the page is not needed to use it.
const snackDone = (ok, doneKey, failKey) => useNotify().snack(t(ok ? doneKey : failKey))
const postOk = async (path) => { try { const r = await apiFetch(path, { method: 'POST' }); return r.ok } catch { return false } }
const reopenGuide = async () => {
  try {
    const config = await apiJson('/api/config')
    config.setup_wizard_completed = false
    Object.assign(config, { guide_done: '', guide_finished: 'false', guide_closed: '', guide_active: '' }) // the guide starts over on every device
    const saved = await apiFetch('/api/config', { method: 'POST', body: config })
    if (!saved.ok) throw new Error('save')
    // the person asked for the guide again: it starts from its first step
    try { window.localStorage.removeItem('foundation.guide.web'); window.sessionStorage.removeItem('foundation.guide.dismissed') } catch { /* the guide resumes where it was */ }
    window.location.href = '/'
  } catch { useNotify().snack(t('troubleshooting.reopen_setup_wizard_error')) }
}
const bigScreenHere = computed(() => canOpenBigScreen({ platform: hostPlatform.value, hostname: window.location.hostname }))
const actionHandlers = computed(() => ({
  restart: async () => { try { await apiFetch('/api/restart', { method: 'POST' }) } catch { /* the connection drops while the host restarts: the offline state takes over */ } },
  connect: () => connect.show('pair'),
  download: () => connect.show('install'),
  logs: () => jumpNav.go('troubleshooting'),
  exportDiag: () => jumpNav.go('troubleshooting'),
  addApp: () => { nav.go('apps'); pending.value = 'addApp' },
  scanGames: () => { nav.go('apps'); pending.value = 'scanGames' },
  stopApp: async () => snackDone(await postOk('/api/apps/close'), 'troubleshooting.force_close_success', 'troubleshooting.force_close_error'),
  ...(hostPlatform.value === 'windows' ? { resetDisplay: async () => snackDone(await postOk('/api/reset-display-device-persistence'), 'troubleshooting.reset_display_device_success_windows', 'troubleshooting.reset_display_device_error_windows') } : {}),
  doctor: () => jumpNav.go('doctor', { autostart: true }),
  // 反馈: the form of MErrorHost, which this shell mounts (an embedded frame has no such shell); 帮助与关于 has the same row
  feedback: () => { useErrors().reportFeedback() },
  setupGuide: reopenGuide,
  quitHost: async () => { try { await apiFetch('/api/boom') } catch { /* the host stops before it answers */ } },
  openDesktop: () => openBigScreenOnHost().catch((e) => { connectNotice.value = `${homeTexts(String(uiLocale.value)).openDesktopFailed}${e?.message ? `: ${e.message}` : ''}` }),
}))
const pageDocsOf = computed(() => Object.entries(PAGES).filter(([id, p]) => p.icon && p.rail !== 'config').filter(onThisHost).map(([id, p]) => ({ id, title: pageLabel(p), icon: p.icon, route: id, path: railLabel(p.rail), keywords: rowKeywords(ROW_OF_PAGE[id]) })))
const searchDocs = computed(() => [
  ...settingDocs([...pageDocsOf.value, ...settingsList.searchItems.value], { prefix: settingsPlace, locale: String(uiLocale.value) }),
  // the big screen opens on the host's own machine: a page anywhere else does not offer the action
  ...actionDocs(String(uiLocale.value), actionHandlers.value, searchT.value.types.action).filter((d) => d.id !== 'action:openDesktop' || bigScreenHere.value),
  ...clientDocs(hostClients.value, { place: railLabel('stream') }),
  ...appDocs(hostApps.value, { place: railLabel('stream') }),
  ...tileDocs(String(uiLocale.value), { exclude: bigScreenHere.value ? [] : ['act-open-desktop'] }),
])
const recentSettingDocs = computed(() => settingDocs(settingsList.recentView.value.items.map((r) => r.entry), { prefix: settingsPlace, locale: String(uiLocale.value) }))
const uni = useUnifiedSearch({
  docs: searchDocs,
  onPin: (doc) => pinToHome('web-home', doc.pin),
  isPinned: (doc) => isPinnedOnHome('web-home', doc.pin),
  recentSettings: recentSettingDocs,
  onChoose: (doc) => {
    if (doc.type === 'setting' || doc.type === 'page') { settingsList.remember(doc.entry?.keyName); onNavigate(doc.entry) }
    else if (doc.type === 'client') { requestPageFilter('pin', doc.client?.name || doc.title); nav.go('pin') }
    else if (doc.type === 'app') { requestPageFilter('apps', doc.app?.name || doc.title); nav.go('apps') }
    else if (doc.type === 'tile') { addTileToHome('web-home', doc.tile); nav.go('home') }
  },
})

// the overview has its own (lighter) frost over the wallpaper; every other page the stronger one
watch(() => nav.current.value, (page) => useAppearance().setScope(page === 'home' || !page ? 'home' : 'other'), { immediate: true })
onMounted(async () => {
  if (embedded) return
  window.addEventListener('resize', onSearchResize)
  void loadSearchData()
  live.start()
  // the appearance of the window (薇薇's wallpaper by default, or the person's choice) is applied once, with the shell
  void useAppearance().init()
  setTimeout(() => update.ensure(), 1500)
  componentStatus.start(60000)
  nav.widen()
  nav.attach()
})
onBeforeUnmount(() => { window.removeEventListener('resize', onSearchResize); nav.detach(); live.stop(); componentStatus.stop() })
</script>

<style>
.web-caption { position: absolute; top: 0; left: var(--m-rail-w, 0px); right: 0; z-index: 40; height: 48px; display: grid; grid-template-columns: minmax(max-content, 1fr) minmax(0, 560px) minmax(max-content, 1fr); align-items: center; transition: left var(--m-spring-default-spatial-duration, .4s) var(--m-spring-default-spatial, ease); }
.web-caption__brand { padding-inline-start: 16px; pointer-events: none; }
.web-caption__search { display: flex; align-items: center; justify-content: center; min-width: 0; padding: 0 12px; }
@media (max-width: 599.98px) { .web-caption { grid-template-columns: minmax(max-content, 1fr) auto auto; } .web-caption__search { padding: 0 8px; } }
</style>
