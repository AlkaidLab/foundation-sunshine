<script setup>
import { computed, inject, onBeforeUnmount, onMounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { DS5_SUB } from 'foundation-ui/settings/index.js'
import { MButton, MDialog, MEmpty, MInfo, MField, MFooter, MGroup, MIcon, MItem, MSection, MSeg, MSkeleton, MBanner, MSnackbar, MStatusPills, MSwitchItem, MChip } from 'foundation-ui/vue'
import { ref } from 'vue'
import { localizedKeyLabel, localizedOptionLabel } from '../../config/dualsenseI18n.js'
import { PRESETS, useDualsenseShared, useDualsenseTuning } from '../../composables/dualsenseShared.js'
import DualSenseLive from './DualSenseLive.vue'
import AdvancedWarningDialog from './AdvancedWarningDialog.vue'
import CfgRow from '../cfg/CfgRow.vue'

// The DualSense block of the gamepad page: what the pad is doing (picture, state), the presets, a card for every group
// of everyday options (with what the values look like) and the advanced groups behind their warning. It sits directly
// under the type cards while DualSense is the chosen type or the host can resolve "automatic" to it; every card and
// group is a sub-page of the gamepad page (the back bubble returns here).
const props = defineProps({ platform: { type: String, default: '' } })
const { t, locale } = useI18n()
const nav = inject('cfgNav')
const store = useDualsenseShared()
const tuning = useDualsenseTuning(t, locale.value)
const { snapshot, edited, loading, loadError, loadWarning, selectedPreset, advancedOpen, cards, advancedGroups, unavailable, valueOf, saveMessage } = store

// Inside the control panel's frame the panel reads what the page cannot (the DualSense transport) and says it here
const embedded = window.isTauri === true && window.parent !== window
const panelStatus = ref(null)
const onFrameMessage = (event) => {
  if (!embedded || event.source !== window.parent || event.data?.type !== 'foundation-ds5-status') return
  panelStatus.value = event.data
}
const openComponents = () => window.parent.postMessage({ type: 'foundation-open', page: 'components' }, '*')
const snack = ref(false)
watch(saveMessage, (text) => { snack.value = text !== '' })

const compiled = computed(() => Boolean(snapshot.value?.backend?.native_path_compiled || snapshot.value?.native_path_compiled))
const controllers = computed(() => Number(snapshot.value?.backend?.active_controllers || 0))
const implemented = computed(() => Number(snapshot.value?.implemented_count || 0))
// the transport the host reports for the connected pad (usb | bluetooth); no pill while the host does not say
const transport = computed(() => String(snapshot.value?.backend?.transport || ''))
const pills = computed(() => {
  if (!compiled.value) return [{ id: 'state', icon: 'info', label: t('ds5.summary.absent'), tone: 'off' }]
  const items = [{ id: 'state', icon: 'check_circle', label: t('ds5.device.ready'), tone: 'ok' }]
  items.push(controllers.value
    ? { id: 'pads', icon: 'sports_esports', label: t('ds5.device.pads', { count: controllers.value }), tone: 'live' }
    : { id: 'pads', icon: 'sports_esports', label: t('ds5.device.none') })
  // the shipped icon set carries no bluetooth glyph; the pill's label names the transport, the glyph only pairs the wireless case with a link
  if (controllers.value && ['usb', 'bluetooth'].includes(transport.value)) items.push({ id: 'transport', icon: transport.value === 'usb' ? 'usb' : 'link', label: t(`ds5.device.transport_${transport.value}`) })
  items.push({ id: 'tunable', icon: 'tune', label: t('ds5.device.tunable', { count: implemented.value }) })
  return items
})

const presetOptions = computed(() => PRESETS.map((name) => ({ value: name, label: t(`ds5.presets.${name}`), icon: { default: 'restart_alt', competitive: 'bolt', immersive: 'graphic_eq', legacy: 'history' }[name] })))
const f2 = (name, fallback = 1) => Number(valueOf(name, fallback)).toFixed(2)
const opt = (name, fallback) => localizedOptionLabel(String(valueOf(name, fallback)), locale.value)
const wired = (card) => card.rows.length > 0
const cardText = (card) => {
  if (!wired(card)) return `${t('ds5.pending.title')} · ${t('ds5.group_count', { count: card.pending.length })}`
  switch (card.id) {
    case 'haptics': return t('ds5.live.haptics', { gain: f2('ds5.haptics.hd.gain'), strength: f2('ds5.rumble.strength') })
    case 'triggers': return t('ds5.live.triggers', { state: valueOf('ds5.trigger.adaptive.enabled', true) === true ? t('ds5.live.on') : t('ds5.live.off'), strength: f2('ds5.trigger.adaptive.strength') })
    case 'motion': return t('ds5.live.motion', { gyro: opt('ds5.gyro.activation', 'hostRequested'), pad: opt('ds5.touchpad.output', 'controller') })
    case 'lighting': return { followGame: t('ds5.color.by_game'), off: t('ds5.color.is_off') }[String(valueOf('ds5.led.lightbar.mode', 'followGame'))] || lightbar.value.toUpperCase()
    case 'audio': return t('ds5.live.audio', { route: opt('ds5.audio.route', 'auto') })
    default: return ''
  }
}

// the advanced groups: behind the five-second warning (never skipped by a reload)
const advancedWarningOpen = ref(false)
const lightbar = computed(() => (String(valueOf('ds5.led.lightbar.mode', 'followGame')) === 'fixed'
  ? '#' + ['ds5.led.lightbar.colorR', 'ds5.led.lightbar.colorG', 'ds5.led.lightbar.colorB'].map((n) => Math.min(255, Math.max(0, Number(valueOf(n, 0)) || 0)).toString(16).padStart(2, '0')).join('')
  : ''))

const restoreAsk = ref(false)
const search = ref('')
const onlyModified = ref(false)
const advancedCount = computed(() => (snapshot.value?.keys || []).filter((key) => key.tier === 'advanced' && key.implemented).length)
const shownGroups = computed(() => {
  const query = search.value.trim().toLowerCase()
  const keep = (key) => {
    if (onlyModified.value && !store.dirty[key.id] && !key.stored) return false
    return !query || key.key.toLowerCase().includes(query) || key.label.toLowerCase().includes(query) || localizedKeyLabel(key, locale.value).toLowerCase().includes(query)
  }
  return advancedGroups.value.map((g) => ({ ...g, wired: g.wired.filter(keep) })).filter((g) => g.wired.length)
})
const toggleAdvanced = () => {
  if (advancedOpen.value) { advancedOpen.value = false; return }
  if (store.warningHidden()) advancedOpen.value = true
  else advancedWarningOpen.value = true
}
const confirmAdvanced = (dontShowAgain) => { store.rememberWarning(dontShowAgain); advancedWarningOpen.value = false; advancedOpen.value = true }
const GROUP_ICON = { 1: 'graphic_eq', 2: 'mobile_vibrate', 3: 'touch_app', 4: 'gamepad', 5: 'tune', 6: 'sensors', 7: 'touch_app', 8: 'lightbulb', 9: 'volume_up', 10: 'verified', 11: 'sync', 12: 'swap_horiz' }
watch(advancedOpen, (open) => { try { localStorage.setItem('ds5.advanced.open', open ? '1' : '0') } catch { /* locked-down browser: the page still works */ } })

onBeforeUnmount(() => window.removeEventListener('message', onFrameMessage))
onMounted(() => {
  if (embedded) { window.addEventListener('message', onFrameMessage); window.parent.postMessage({ type: 'foundation-ds5-status-request' }, '*') }
  try { if (!advancedOpen.value) advancedOpen.value = localStorage.getItem('ds5.advanced.open') === '1' && store.warningHidden() } catch { /* the section stays closed */ }
  tuning.load()
})
</script>

<template>
  <div class="ds5-block">
    <div class="ds5-device" data-cfg="ds5-block">
      <div class="ds5-device__body">
        <h3 class="ds5-device__title">DualSense<MInfo v-if="platform === 'windows'" :text="t('ds5.device.apply_note')" /></h3>
        <MSkeleton v-if="loading && !snapshot" :rows="1" />
        <MStatusPills v-else :items="pills" :label="t('ds5.device.title')" />
      </div>
    </div>

    <MBanner v-if="panelStatus?.needsTransport" tone="warn" icon="usb">
      {{ panelStatus.label }}<template v-if="panelStatus.next"> · {{ panelStatus.next }}</template>
      <template #actions><MButton variant="text" size="sm" @click="openComponents">{{ t('ds5.device.open_components') }}</MButton></template>
    </MBanner>
    <MBanner v-if="loadError" tone="error" icon="error">
      {{ t('ds5.load_failed') }}: {{ loadError }}
      <template #actions><MButton variant="text" size="sm" @click="tuning.load(true)">{{ t('ds5.retry') }}</MButton></template>
    </MBanner>
    <MBanner v-else-if="loadWarning" tone="warn">{{ loadWarning }}</MBanner>

    <template v-if="snapshot">
      <MSection :title="t('ds5.presets.title')" data-cfg="ds5:ds5.profile.preset">
        <MSeg block :label="t('ds5.presets.title')" :model-value="selectedPreset" :options="presetOptions" @update:model-value="tuning.applyPreset" />
        <MFooter>{{ selectedPreset !== 'custom' ? t('ds5.presets.' + selectedPreset + '_hint') : t('ds5.presets.custom') }}</MFooter>
      </MSection>

      <MSection :title="t('ds5.common')">
        <MGroup>
          <MItem v-for="card in cards" :key="card.id" action :icon="card.icon" :title="t(card.titleKey)" :text="cardText(card)" :class="{ 'ds5-card--idle': !wired(card) }" :data-cfg-sub="DS5_SUB.card(card.id)" @click="nav.open(DS5_SUB.card(card.id))">
            <template #trail><DualSenseLive :id="card.id" compact /></template>
          </MItem>
        </MGroup>
      </MSection>

      <MSection>
        <MGroup>
          <MItem action icon="tune" :title="t('ds5.gamepad_advanced')" :text="t('ds5.advanced_hint', { count: advancedCount })" :aria-expanded="advancedOpen" @click="toggleAdvanced">
            <template #trail><MIcon :name="advancedOpen ? 'keyboard_arrow_up' : 'keyboard_arrow_down'" /></template>
          </MItem>
        </MGroup>
        <div v-if="advancedOpen" class="ds5-advanced">
          <MField v-model="search" :label="t('ds5.search')" />
          <MGroup>
            <MSwitchItem v-model="onlyModified" :title="t('ds5.only_modified')" />
          </MGroup>
          <MEmpty v-if="!shownGroups.length" icon="search" :title="t('ds5.no_matches')" />
          <MGroup v-else>
            <MItem v-for="entry in shownGroups" :key="entry.id" action :icon="GROUP_ICON[entry.id] || 'tune'" :title="t(entry.titleKey)" :text="t('ds5.group_count', { count: entry.wired.length + entry.pending.length })" :data-cfg-sub="DS5_SUB.group(entry.id)" @click="nav.open(DS5_SUB.group(entry.id))">
            </MItem>
          </MGroup>
          <MGroup v-if="unavailable.total && !search">
            <MItem action icon="schedule" :title="t('ds5.pending.title')" :text="t('ds5.group_count', { count: unavailable.total })" :data-cfg-sub="DS5_SUB.pending" @click="nav.open(DS5_SUB.pending)" />
          </MGroup>
          <MGroup>
            <MItem action icon="restart_alt" :title="t('ds5.restore_defaults')" @click="restoreAsk = true" />
          </MGroup>
        </div>
      </MSection>

      <MSection>
        <MGroup>
          <CfgRow id="ds5-test" :title="t('ds5.test_panel.title')" icon="gamepad" />
        </MGroup>
      </MSection>
    </template>

    <MDialog v-model="restoreAsk" :title="t('ds5.restore_defaults')" icon="restart_alt" tone="info">
      <p>{{ t('ds5.restore_confirm') }}</p>
      <template #actions>
        <MButton variant="text" @click="restoreAsk = false">{{ t('_common.cancel') }}</MButton>
        <MButton @click="restoreAsk = false; tuning.restoreDefaults()">{{ t('ds5.restore_defaults') }}</MButton>
      </template>
    </MDialog>
    <AdvancedWarningDialog :show="advancedWarningOpen" @cancel="advancedWarningOpen = false" @confirm="confirmAdvanced" />
    <MSnackbar v-model="snack" :message="saveMessage" />
  </div>
</template>

<style>
.ds5-block { display: contents; }
.ds5-device { display: flex; align-items: center; gap: 20px; padding: 16px 20px; border-radius: 28px; background: var(--m-card, var(--md-sys-color-surface-container-low)); }
.ds5-device__body { display: flex; flex-direction: column; gap: 8px; min-width: 0; flex: 1; }
.ds5-device__title { margin: 0; display: flex; align-items: center; gap: 6px; font: var(--m-title-large); color: var(--md-sys-color-on-surface); }
.ds5-card--idle .m-item__title { color: var(--md-sys-color-on-surface-variant); }
.ds5-advanced { display: contents; }
</style>
