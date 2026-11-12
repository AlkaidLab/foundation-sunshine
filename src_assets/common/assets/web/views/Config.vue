<template>
  <div class="cfg-page">
    <MSkeleton v-if="!config" :rows="6" />

    <MPage v-else :title="pageTitle" :back="canBack" :back-label="$t('shell.back')" @back="goBack">
      <MBanner v-if="validationError" tone="error" icon="error">
        {{ $t(validationError) }}
        <template #actions>
          <MButton variant="text" size="sm" @click="goToValidation">{{ validationError.startsWith('connections.') ? $t('connections.network_link') : `${$t('tabs.amd')} · ${$t('config.amd_advanced_group')}` }}</MButton>
        </template>
      </MBanner>

      <MAppearance v-if="tab === 'appearance'" :locale="String(uiLocale)" />
      <Suspense v-else>
        <template #fallback><MSkeleton :rows="6" /></template>
        <div :key="`${tab}:${subNav.current.value}`" class="cfg-tab">
          <General v-if="tab === 'general'" :config="config" :global-prep-cmd="global_prep_cmd" :platform="platform" />
          <Inputs v-else-if="tab === 'input'" :config="config" :platform="platform" />
          <Display v-else-if="tab === 'display'" :config="config" :platform="platform" :resolutions="resolutions" :fps="fps" :encoders="encoderTabs" :focus="focus" />
          <Audio v-else-if="tab === 'audio'" :config="config" :platform="platform" />
          <Network v-else-if="tab === 'network'" :config="config" />
          <Files v-else-if="tab === 'files'" :config="config" :platform="platform" />
        </div>
      </Suspense>
    </MPage>

  </div>
</template>

<script setup>
import { computed, h, inject, nextTick, onMounted, onUnmounted, provide, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MAppearance, MBanner, MButton, MPage, MSkeleton, pinSpecForSetting, registerContext, showTip, useSettingsList, useSubpage, jumpToRow } from 'foundation-ui/vue'
import { DEFAULT_CATEGORY, DEFAULT_PLACE, PLACE_TABS, SEARCH_INDEX, parentPlace, parseSettingsHash, settingsHash } from 'foundation-ui/settings/index.js'
import { useSharedConfig } from '../composables/configStore.js'
import { encoderTabsFor } from '../config/configTabs.js'
import { lazyPane } from '../utils/lazyPane.js'

const TabLoading = () => h(MSkeleton, { rows: 6 })
const tabChunk = (loader) => lazyPane(loader, { loading: TabLoading })
const General = tabChunk(() => import('../configs/tabs/General.vue'))
const Inputs = tabChunk(() => import('../configs/tabs/Inputs.vue'))
const Network = tabChunk(() => import('../configs/tabs/Network.vue'))
const Files = tabChunk(() => import('../configs/tabs/Files.vue'))
const Audio = tabChunk(() => import('../configs/tabs/Audio.vue'))
const Display = tabChunk(() => import('../configs/tabs/Display.vue'))

// The shell hands over where to land: { tab, sub, focus } (a search hit or a link with a hash).
const props = defineProps({ params: { type: Object, default: null } })
const { t, locale: uiLocale } = useI18n()
const webShell = inject('webShell', null)

const {
  validationError,
  platform,
  config,
  fps,
  resolutions,
  global_prep_cmd,
  tabs,
  initTabs,
  loadConfig,
  markKeySaved,
} = useSharedConfig()

// Settings the person changes join the recently used list (the control panel's list pane keeps its own copy: the frame tells it)
const settingsList = useSettingsList({ locale: useI18n().locale })
const indexed = new Set(SEARCH_INDEX.map((e) => e.id))
let seen = null
const noteChanges = () => {
  const now = {}
  for (const [key, value] of Object.entries(config.value || {})) if (indexed.has(key)) now[key] = JSON.stringify(value)
  if (seen) {
    for (const key of Object.keys(now)) {
      if (seen[key] !== now[key]) {
        settingsList.remember(key)
        if (embedded) window.parent.postMessage({ type: 'foundation-settings-recent', key }, '*')
      }
    }
  }
  seen = now
}
const embedded = window.isTauri === true && window.parent !== window
watch(config, noteChanges, { deep: true, flush: 'post' })

// the context menu of a setting's row (SPEC-context-menu, "Setting row"): the way back to the default, the link to the setting, the search that finds it, 这是什么 (the row's own info icon, shown the way a tap on it shows it)
// and the pin to the overview (the pin menu of a row: the product's one MPinHost); the tip comes after the menu has closed, the tip hides when the focus moves. A key that is not in the file is the default already.
const pinning = inject('m-pin', null)
const offSettingMenu = registerContext('setting', ({ id, el, T, cb }) => {
  const entry = SEARCH_INDEX.find((e) => e.id === id)
  if (!entry) return []
  const info = el?.querySelector?.('.m-info__btn')
  const spec = pinning ? pinSpecForSetting(id, { title: el?.querySelector?.('.m-item__title')?.textContent?.trim() || '', locale: String(uiLocale.value) }) : null
  return [
    ...(info ? [{ id: 'what-is-this', label: T.whatIsThis, icon: 'help', run: () => setTimeout(() => { info.focus({ preventScroll: true }); showTip(info) }, 300) }] : []),
    ...(spec ? [{ id: 'pin-setting', label: pinning.isPinned(spec) ? T.unpin : T.pin, icon: 'keep', run: () => pinning.toggle(spec) }] : []),
    ...(config.value && id in config.value ? [{ id: 'restore-default', label: T.restoreDefault, icon: 'restart_alt', run: () => { delete config.value[id] } }] : []),
    { id: 'copy-setting-link', label: T.copySettingLink, icon: 'link', run: () => navigator.clipboard?.writeText(`${location.origin}${location.pathname}${settingsHash(entry.params)}`).then(() => cb.notice?.(T.copied)).catch(() => {}) },
    ...(cb.search ? [{ id: 'locate-in-search', label: T.locateInSearch, icon: 'search', run: () => cb.search(id) }] : []),
  ]
})

// Where we are: a tab (from the list pane or a link), a section of it (a page of its own) inside
const tabNav = useSubpage('cfgtab')
const subNav = useSubpage('cfgsub')
const subTitle = ref('')
const focus = ref(props.params?.focus || '')
// The settings never open empty: without a choice the first category (常规) shows
// the panel's 手柄 destination is this page opened at the gamepad: it carries the gamepad's name
const asGamepad = ref(false)
const tab = computed(() => props.params?.tab || tabNav.current.value || DEFAULT_CATEGORY)
const encoderTabs = computed(() => encoderTabsFor(tabs.value))
// A section (sub) belongs to its tab. One left in the window's history by another tab (the frame keeps it when the panel moves to this tab) hides every row of this one: CfgSub shows its rows only
// while no section is open: an empty page with only a Back button. These tabs have the sections below and no others, so any other is dropped.
const OWN_SUBS = { general: ['expert', 'prep'], network: ['expert', 'webhook'], files: [] }
watch(() => [tab.value, subNav.current.value], ([id, current]) => { if (current && OWN_SUBS[id] && !OWN_SUBS[id].includes(current)) nextTick(() => { if (subNav.current.value === current) subNav.open('', true) }) }, { immediate: true })
provide('cfgMarkSaved', markKeySaved)
provide('cfgNav', { sub: subNav.current, open: subNav.open, title: subTitle })
provide('platform', computed(() => platform.value))

// the sections that are part of the tab page (CfgSub inline): an address with one of them opens the page and jumps to the row
const INLINE_SUBS = { general: ['expert', 'prep'], network: ['expert'] }
const openSub = (id, forTab = tab.value, replace = false) => { if (id && !INLINE_SUBS[forTab]?.includes(id)) subNav.open(id, replace) }
// the places of 显示与视频, 音频 and 输入与外设 are the rows of their list: the page is the place, and Back belongs to the shell where the list is hidden behind it (single pane), never beside the list.
// A page inside a place (the gamepad's per-game page, a DualSense group) has its own Back, to the place.
const isPlace = computed(() => !!PLACE_TABS[tab.value])
const canBack = computed(() => !!subNav.current.value && !(isPlace.value && parentPlace(tab.value, subNav.current.value) === subNav.current.value))
// a tab of places never opens empty: without a place, or with a page that belongs to none (an address of another version), the first place shows; such a page would be an empty one with only a Back button
watch(() => [tab.value, subNav.current.value], ([id, current]) => { if (PLACE_TABS[id] && !parentPlace(id, current)) nextTick(() => { if (!parentPlace(id, subNav.current.value)) subNav.open(DEFAULT_PLACE[id], true) }) }, { immediate: true })
// inside the control panel's frame: tell the panel when a sub-page (with its own back bubble) is open; a place has none, the panel's own bar is the way back
watch(canBack, (open) => { if (embedded) window.parent.postMessage({ type: 'foundation-subpage', open }, '*') }, { immediate: true })
const goBack = () => { const parent = isPlace.value ? parentPlace(tab.value, subNav.current.value) : ''; if (parent && parent !== subNav.current.value) subNav.open(parent); else subNav.back() }
const openTab = (id) => tabNav.open(id)
// the title is the section's own once it is open; until that section has said its name (its chunk is still loading) there is no title, never the name of the tab or of the section before
const pageTitle = computed(() => {
  if (subNav.current.value) return subTitle.value
  if (((embedded && asGamepad.value) || props.params?.focus === 'ds5-block') && tab.value === 'input') return t('config.input_gamepad_title')
  return tab.value === 'appearance' ? t('shell.appearance') : t(`tabs.${tab.value}`)
})
watch(() => tabNav.current.value, (id) => { if (!id) subNav.current.value = '' })
watch(() => [tab.value, subNav.current.value], () => { subTitle.value = '' }, { flush: 'sync' })
// a page opens at its top, whatever the one before was scrolled to
watch(() => subNav.current.value, () => nextTick(() => {
  const page = document.querySelector('.cfg-page')
  for (const el of [...(page?.querySelectorAll('*') || []), ...(function* () { for (let a = page; a; a = a.parentElement) yield a })()]) if (el.scrollTop > 0) el.scrollTo({ top: 0 })
  window.scrollTo({ top: 0 })
}))

// An error that belongs to a setting takes the user there
function goToValidation() {
  const target = validationError.value.startsWith('connections.') ? { tab: 'network', sub: 'connect' } : { tab: 'display', sub: 'encoders', focus: 'amd_rc' }
  if (webShell?.nav) webShell.nav.go('config', target)
  else { openTab(target.tab); nextTick(() => subNav.open(target.sub)) }
}

// Land on the section and the setting of a search hit: find the row, scroll the pane that holds the page (not the document) and pulse it
const landOn = (focus) => { if (focus) window.dispatchEvent(new CustomEvent('foundation-focus', { detail: { focus } })); if (focus) void jumpToRow({ selector: `.cfg-tab [data-cfg="${CSS.escape(focus)}"]` }) }

const onHash = () => {
  const target = parseSettingsHash(window.location.hash, SEARCH_INDEX)
  if (!target) return
  focus.value = target.focus || ''
  // inside the control panel's frame there is no shell stack: the page opens the tab and the section itself
  if (webShell?.nav && !embedded) { if (webShell.openConfig) webShell.openConfig(target); else webShell.nav.go('config', target); return }
  asGamepad.value = target.focus === 'ds5-block'
  openTab(target.tab)
  if (target.sub) nextTick(() => openSub(target.sub, target.tab))
  if (target.focus) landOn(target.focus)
}

// The control panel's list pane drives this page through the frame: { type: 'foundation-settings-go', tab, sub, focus }
const onFrameMessage = (event) => {
  const data = event.data
  if (!embedded || event.source !== window.parent || data?.type !== 'foundation-settings-go' || !data.tab) return
  asGamepad.value = data.focus === 'ds5-block'
  focus.value = data.focus || ''
  tabNav.open(data.tab)
  subNav.current.value = ''
  if (data.sub) nextTick(() => openSub(data.sub, data.tab))
  if (data.focus) landOn(data.focus)
}
onMounted(async () => {
  window.addEventListener('message', onFrameMessage)
  if (!config.value) {
    initTabs()
    await loadConfig()
  }
  if (props.params?.sub && subNav.current.value !== props.params.sub) openSub(props.params.sub, tab.value, true)
  if (props.params?.focus) { focus.value = props.params.focus; landOn(props.params.focus) }
  else if (!props.params && window.location.hash) onHash()
  window.addEventListener('hashchange', onHash)
})
onUnmounted(() => { window.removeEventListener('hashchange', onHash); window.removeEventListener('message', onFrameMessage); offSettingMenu() })
</script>

<style lang="less">
.cfg-tab { display: contents; }
.cfg-sub { display: contents; }
</style>
