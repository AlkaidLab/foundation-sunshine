<script setup>
import { computed, inject, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import QRCode from 'qrcode'
import { MBanner, MButton, MDialog, MFooter, MGroup, MItem, MReachOverview, MRemoteWindow, MSection, MWebAccess, endpointsOfConfig, lanView, reachTexts, remoteTexts, webAccessTexts } from 'foundation-ui/vue'
import { useReach } from '../../composables/useReach.js'
import { useGuide } from '../../composables/useGuide.js'
import CfgChoice from '../../components/cfg/CfgChoice.vue'
import CfgExpert from '../../components/cfg/CfgExpert.vue'
import CfgSlider from '../../components/cfg/CfgSlider.vue'
import NetworkPorts from './network/NetworkPorts.vue'
import { isChanged } from 'foundation-ui/settings/index.js'
import CfgField from '../../components/cfg/CfgField.vue'
import CfgRow from '../../components/cfg/CfgRow.vue'
import CfgSub from '../../components/cfg/CfgSub.vue'
import CfgSwitch from '../../components/cfg/CfgSwitch.vue'
import CfgTop from '../../components/cfg/CfgTop.vue'
import { apiJson } from '../../utils/apiFetch.js'
import WebhookCard from '../webhook/WebhookCard.vue'

const props = defineProps(['config'])
const { t, locale } = useI18n()
// the remote access card: the host's own check (public internet and mesh), with the fix of each reason where this page can do it
const reach = useReach()
const rc = computed(() => reachTexts(String(locale.value)))
const rm = computed(() => remoteTexts(String(locale.value)))
const guide = useGuide()
const markSaved = inject('cfgMarkSaved', (key, value) => { config.value[key] = value })
const addressBusy = ref(false)
const saveAddress = async (value) => {
  addressBusy.value = true
  try {
    const r = await guide.callbacks.setPublicEndpoints(value)
    if (r.ok) { markSaved("external_endpoints", value); markSaved("external_ip", ""); await reach.refresh(true) }
  } finally { addressBusy.value = false }
}
const upnpAsk = ref(false)
const restartAsk = ref(false)
const restartNow = () => { restartAsk.value = false; void guide.callbacks.restartHost() }
const focusKey = (key) => { const el = document.querySelector(`[data-cfg="${key}"]`); el?.scrollIntoView({ block: 'center', behavior: 'smooth' }); el?.querySelector?.('button, input, select')?.focus?.() }
const reachFix = (action) => {
  if (action === 'upnp') upnpAsk.value = true
  else if (action === 'origin') focusKey('origin_web_ui_allowed')
  else if (action === 'restart') restartAsk.value = true
  else openTutorial(action)
}
const upnpOn = () => { upnpAsk.value = false; config.value.upnp = 'enabled'; focusKey('upnp') }
const tutorial = ref(false)
const tutorialMode = ref('')
const openTutorial = (action) => { tutorialMode.value = action === 'mesh' ? 'mesh' : 'public'; tutorial.value = true }
const platform = computed(() => String(props.config?.platform || 'windows').toLowerCase())
onMounted(() => { void reach.refresh() })
const defaultMoonlightPort = 47989
const config = ref(props.config)
const scope = computed({ get: () => config.value.origin_web_ui_allowed ?? 'lan', set: (value) => { config.value.origin_web_ui_allowed = value } })
const bind = computed({ get: () => config.value.bind_address ?? '', set: (value) => { config.value.bind_address = value } })
const qrMatrix = (text) => QRCode.create(text, { errorCorrectionLevel: 'M' }).modules
const copyText = (text) => navigator.clipboard.writeText(text)
const goPassword = () => { window.location.href = '/password' }
const openBind = String(window.location.hash).includes('bind_address')
const listenerInfo = ref(null)
const listenerError = ref('')
let listenerRequest = 0
const activeListener = computed(() => listenerInfo.value?.active)
// the local network beside the internet and the mesh: the listener in use as the host reports it
const lan = computed(() => lanView(listenerInfo.value, config.value, String(locale.value), listenerError.value))
const savedListener = computed(() => listenerInfo.value?.saved)
const listenerFamilyLabel = (settings) => {
  if (settings.bind_address) return settings.bind_address.includes(':') ? 'IPv6' : 'IPv4'
  return settings.address_family === 'both' ? 'IPv4 + IPv6' : 'IPv4'
}
const listenerPending = computed(
  () =>
    activeListener.value &&
    savedListener.value &&
    ['bind_address', 'address_family', 'port', 'upnp', 'origin_web_ui_allowed'].some(
      (key) => (key !== 'origin_web_ui_allowed' || activeListener.value[key] !== undefined) && String(activeListener.value[key]) !== String(savedListener.value[key]),
    ),
)
const loadListeners = async () => {
  const request = ++listenerRequest
  try {
    const data = await apiJson('/api/network/listeners')
    if (request !== listenerRequest) return
    if (data.status !== true) throw new Error(data.error || 'Unable to enumerate listeners')
    listenerInfo.value = data
    listenerError.value = ''
  } catch (error) {
    if (request === listenerRequest) listenerError.value = error.message
  }
}
const effectivePort = computed(() => {
  const port = config.value?.port
  return port === undefined || port === null || port === '' ? defaultMoonlightPort : Number(port)
})

onMounted(() => {
  void loadListeners()
  window.addEventListener('sunshine:config-saved', loadListeners)
})
onBeforeUnmount(() => {
  listenerRequest++
  window.removeEventListener('sunshine:config-saved', loadListeners)
})

const listenerText = (settings) => `${settings.bind_address || t('connections.all_addresses')} · ${listenerFamilyLabel(settings)} · ${settings.port}`
const connectText = computed(() => (activeListener.value ? listenerText(activeListener.value) : ''))
const proposedText = computed(() => `${config.value.bind_address || t('connections.all_addresses')} · ${listenerFamilyLabel(config.value)} · ${effectivePort.value}`)
const familyOptions = computed(() => [{ value: 'ipv4', label: t('config.address_family_ipv4') }, { value: 'both', label: t('config.address_family_both') }])
const lanOptions = computed(() => [{ value: '0', label: t('_common.disabled') }, { value: '1', label: t('config.lan_encryption_mode_1') }, { value: '2', label: t('config.lan_encryption_mode_2') }])
const wanOptions = computed(() => [{ value: '0', label: t('_common.disabled') }, { value: '1', label: t('config.wan_encryption_mode_1') }, { value: '2', label: t('config.wan_encryption_mode_2') }])
const portRows = computed(() => {
  const base = +effectivePort.value
  return [
    { id: 'https', protocol: t('config.port_tcp'), port: String(base - 5), note: 'HTTPS API' },
    { id: 'http', protocol: t('config.port_tcp'), port: String(base), note: 'HTTP API' },
    { id: 'web', protocol: t('config.port_tcp'), port: String(base + 1), note: t('config.port_web_ui') },
    { id: 'rtsp', protocol: t('config.port_tcp'), port: String(base + 21), note: 'RTSP' },
    { id: 'stream', protocol: t('config.port_udp'), port: `${base + 9} - ${base + 12}`, note: t('config.network_stream_ports') },
  ]
})
</script>

<template>
  <CfgTop>
    <MSection :title="webAccessTexts(String(locale)).title">
      <MWebAccess v-model:scope="scope" v-model:bind="bind" :locale="String(locale)" :port="+effectivePort" :listeners="listenerInfo" :password="true" :pending="!!listenerPending" :qr="qrMatrix" :copy="copyText" :open="openBind" @restart="restartAsk = true" @password="goPassword" />
    </MSection>
    <MSection :title="rc.overview.label">
      <MReachOverview address-editable data-cfg="external_endpoints" :label="$t('config.external_endpoints')" :endpoints="endpointsOfConfig(config).join(', ')" :address-busy="addressBusy" :report="reach.view.report" :status="reach.view.status" :http="reach.view.http" :cause="reach.view.cause" :lan="lan" :locale="String(locale)" :platform="platform" @recheck="reach.refresh(true)" @fix="reachFix" @save-address="saveAddress" />
      <MButton variant="text" icon="help" @click="openTutorial('')">{{ rc.tutorial }}</MButton>
    </MSection>
    <MSection :title="$t('config.network_port_details')">
      <NetworkPorts :config="config" :base="+effectivePort" :listener="activeListener" />
      <MBanner v-if="+effectivePort - 5 < 1024" tone="error" icon="error">{{ $t('config.port_alert_1') }}</MBanner>
      <MBanner v-if="+effectivePort + 21 > 65535" tone="error" icon="error">{{ $t('config.port_alert_2') }}</MBanner>
    </MSection>

    <MSection>
      <MGroup>
        <CfgField :config="config" k="port" type="number" min="1029" max="65514" :placeholder="defaultMoonlightPort" />
        <CfgSwitch :config="config" k="mdns_broadcast" def="enabled" />
        <CfgSwitch :config="config" k="upnp" def="disabled" />
        <CfgChoice :config="config" k="wan_encryption_mode" :options="wanOptions" def="1" />
        <CfgExpert :config="config" k="lan_encryption_mode" control="seg" :options="lanOptions" def="0" />
        <CfgSwitch :config="config" k="fec_auto" def="disabled" />
      </MGroup>
    </MSection>

    <MSection>
      <MGroup>
        <MItem href="/pin#client-list" icon="devices" :title="$t('connections.manage_clients')" />
        <CfgRow id="webhook" title="Webhook" icon="notifications" />
      </MGroup>
    </MSection>
  </CfgTop>

  <CfgSub id="expert" :title="$t('tabs.advanced')" inline>
    <MBanner v-if="listenerPending" tone="warn" icon="warning">{{ $t('connections.pending_restart') }}</MBanner>
    <MSection :title="$t('config.network_connect_title')">
      <MGroup>
        <MItem v-if="activeListener" icon="check_circle" :title="$t('connections.active')" :text="listenerText(activeListener)" />
        <CfgExpert :config="config" k="address_family" control="seg" :options="familyOptions" def="ipv4" />
      </MGroup>
      <MFooter v-if="listenerError">{{ $t('connections.address_list_failed') }} {{ listenerError }}</MFooter>
      <MFooter>{{ $t('connections.proposed') }}: <code>{{ proposedText }}</code> · {{ $t('connections.source_note') }}</MFooter>
      <MFooter>{{ $t('config.network_manual_desc') }}<template v-if="effectivePort !== defaultMoonlightPort"> {{ $t('config.port_http_port_note') }}</template></MFooter>
    </MSection>
    <MSection :title="$t('config.network_streaming_title')">
      <MGroup>
        <CfgExpert :config="config" k="fec_percentage" min="0" max="255" />
        <CfgExpert :config="config" k="fec_auto_max_percentage" />
      </MGroup>
      <MFooter>{{ $t('config.fec_apply_note') }}</MFooter>
    </MSection>
    <MSection :title="$t('config.network_pairing_title')">
      <MGroup>
        <CfgExpert :config="config" k="pair_max_attempts" />
        <CfgExpert :config="config" k="ping_timeout" />
        <CfgExpert :config="config" k="close_verify_safe" />
      </MGroup>
    </MSection>
  </CfgSub>

  <CfgSub id="webhook" title="Webhook"><WebhookCard /></CfgSub>

  <MDialog v-model="upnpAsk" :title="rm.confirmTitle" icon="warning" tone="warning">
    <p>{{ rm.confirmText }}</p>
    <template #actions>
      <MButton variant="text" @click="upnpAsk = false">{{ rm.confirmNo }}</MButton>
      <MButton @click="upnpOn">{{ rm.confirmYes }}</MButton>
    </template>
  </MDialog>
  <MDialog v-model="restartAsk" :title="rm.restart" icon="restart_alt" tone="warning">
    <p>{{ rm.restartNote }}</p>
    <template #actions>
      <MButton variant="text" @click="restartAsk = false">{{ rm.confirmNo }}</MButton>
      <MButton @click="restartNow">{{ rm.restart }}</MButton>
    </template>
  </MDialog>
  <MRemoteWindow v-model="tutorial" :locale="String(locale)" :state="guide.state" :callbacks="guide.callbacks" :initial-mode="tutorialMode" />
</template>
