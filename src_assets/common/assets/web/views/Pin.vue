<template>
  <div class="pin-page">
    <MPage :title="$t('connections.title')" :updated="clientsAt" :refreshing="loading" refreshable :locale="String(locale)" @refresh="refreshClients()">
      <MBanner v-if="listError" tone="error" icon="error">{{ listError }}</MBanner>
      <MBanner v-else-if="clientError && !showPairing && !selectedClient && !confirmAction" tone="error" icon="error">{{ clientError }}</MBanner>

      <!-- pairing is the first thing on the page: a PIN or a QR code -->
      <div class="pin-pairing">
        <MPromo tone="secondary" icon="dialpad" :title="tx.pinPair" :text="tx.pinPairText" @click="openPairing('pin')" />
        <MPromo tone="secondary" icon="qr_code_2" :title="tx.qrPair" :text="tx.qrPairText" @click="openPairing('qr')" />
      </div>

      <MSection :title="$t('connections.paired')">
        <template #action><span class="pin-count">{{ clients.length }}</span></template>
        <MSkeleton v-if="(loading || !listed) && !clients.length" :rows="3" />
        <MEmpty v-else-if="!clients.length" icon="devices" :title="$t('connections.empty')" />
        <template v-else>
          <div class="pin-filter">
            <MField v-model="query" class="pin-filter__search" :label="tx.search" type="search" />
            <MSeg v-model="kind" class="pin-filter__kind" :label="tx.filter" :options="kindChoices" />
          </div>
          <MEmpty v-if="!shown.length" icon="search_off" :title="tx.noMatch" />
          <section v-for="group in groups" :key="group.kind" class="pin-group">
            <h3 v-if="groups.length > 1" class="pin-group__title">{{ tx[group.kind] || tx.other }}<span class="pin-count">{{ group.items.length }}</span></h3>
            <div class="pin-list" role="list" @keydown="rowKeys">
              <div
                v-for="client in group.items"
                :key="client.uuid"
                class="m-item m-split-item pin-row"
                :data-row="client.name"
                :class="{ 'm-item--disabled': client.enabled === false }"
                role="listitem"
                @contextmenu="onRowMenu($event, client)"
              >
                <div class="m-split-item__main pin-row__main">
                  <span class="m-item__lead"><span class="m-lead-circle m-shaped" :class="[leadTone(client), `m-shaped--${shapeFor(client)}`]"><MIcon :name="deviceIconFor(client.name, client.os)" /></span></span>
                  <span class="m-item__body">
                    <span class="m-item__title" data-redact="client">{{ client.name || $t('pin.unknown_client') }}</span>
                    <span v-if="rowText(client)" class="m-item__text" :class="{ 'pin-row__live': stateOf(client) === 'streaming' }">{{ rowText(client) }}</span>
                  </span>
                </div>
                <MSwitch
                  class="pin-row__switch"
                  :model-value="client.enabled !== false"
                  :disabled="updating.has(client.uuid) || loading || saving"
                  :label="$t('connections.allow_device', { name: client.name })"
                  @update:model-value="(value) => requestToggle(client, value)"
                />
                <i class="m-split-item__divider" aria-hidden="true" />
                <button type="button" class="m-split-item__gear" :disabled="updating.has(client.uuid)" :aria-label="$t('connections.settings_for', { name: client.name })" :data-tip="$t('connections.device_settings')" @click="editClient(client)"><MIcon name="settings" /></button>
              </div>
            </div>
          </section>
        </template>
        <MFooter v-if="clients.length">{{ $t('connections.disable_note') }}</MFooter>
      </MSection>

      <MGroup>
        <MItem href="/config#bind_address" icon="router" :title="$t('connections.network_question')" :text="$t('connections.network_hint')" />
      </MGroup>

      <MSection :title="$t('connections.danger_zone')">
        <MGroup>
          <MItem icon="delete" :title="$t('troubleshooting.unpair_all')">
            <template #trail>
              <MButton variant="danger" size="sm" :disabled="actionBusy || !clients.length" @click="confirmAction = { type: 'unpair-all' }">{{ $t('troubleshooting.unpair_all') }}</MButton>
            </template>
          </MItem>
        </MGroup>
      </MSection>
    </MPage>

    <!-- Pairing: PIN pad or QR code -->
    <MSheet :model-value="showPairing" :title="$t('connections.add')" side="end" :close-label="$t('_common.close')" @update:model-value="(open) => !open && closePairing()">
      <div class="pin-pair">
        <MSeg block :model-value="pairMode" :label="$t('connections.pair_method')" :options="[{ value: 'pin', label: $t('pin.or_manual_pin'), icon: 'dialpad' }, { value: 'qr', label: $t('pin.qr_pairing'), icon: 'qr_code_2' }]" @update:model-value="setPairMode" />

        <div v-show="pairMode === 'pin'" id="pair-pin-panel" role="tabpanel" class="pin-pair__pin">
          <div class="pin-pair__wait"><MScallop :busy="pairingBusy" :label="$t('connections.waiting_client')"><MIcon name="dialpad" /></MScallop><span>{{ pairingBusy ? $t('connections.pairing') : $t('connections.waiting_client') }}</span></div>
          <p class="pin-pair__hint">{{ $t('connections.pin_help') }}</p>
          <div class="pin-pair__name" data-redact="client">
            <MField v-model="pairingDeviceName" :label="$t('pin.device_name')" :disabled="pairingBusy" @keydown.enter="submitPairing" />
          </div>
          <MPinPad data-redact="pin" v-model="pinValue" :length="PIN_LENGTH" label="PIN" :backspace-label="$t('pin.delete_digit')" :disabled="pairingBusy" />
          <p v-if="clientError" class="pin-pair__error" role="alert">{{ clientError }}</p>
        </div>

        <div v-show="pairMode === 'qr'" id="pair-qr-panel" role="tabpanel" class="pin-pair__qr">
          <template v-if="qrActive">
            <img :src="qrDataUrl" class="pair-qr-image" alt="QR Code" data-redact="pin" />
            <p class="pin-pair__qrpin" data-redact="pin">PIN <b>{{ qrPin }}</b></p>
            <p class="pin-pair__hint">{{ $t('pin.qr_expires_in', { seconds: qrRemaining }) }}</p>
          </template>
          <p v-if="qrError" class="pin-pair__error" role="alert">{{ qrError }}</p>
          <MButton variant="tonal" icon="qr_code_2" :disabled="qrLoading" @click="generateQrCode">{{ $t(qrActive ? 'pin.qr_refresh' : 'pin.qr_generate') }}</MButton>
          <MBanner tone="warn">{{ $t('pin.qr_pairing_warning') }}</MBanner>
        </div>
      </div>
      <template v-if="pairMode === 'pin'" #actions>
        <MButton class="pin-pill" icon="check" :loading="pairingBusy" :disabled="!pairReady" @click="submitPairing">{{ $t('pin.send') }}</MButton>
      </template>
    </MSheet>

    <!-- Device settings: side sheet on wide windows, bottom sheet on narrow ones -->
    <MSheet :model-value="!!selectedClient" :title="$t('connections.device_settings')" side="end" :close-label="$t('_common.close')" @update:model-value="(open) => !open && !saving && closeDevice()">
      <div v-if="selectedClient" class="pin-device">
        <div class="pin-device__head">
          <span class="m-lead-circle" :class="leadTone(selectedClient)"><MIcon :name="deviceIconFor(selectedClient.name, selectedClient.os)" /></span>
          <MField data-redact="client" v-model="selectedClient.name" :label="$t('pin.device_name')" />
        </div>

        <MSection :title="$t('connections.display_settings')">
          <MGroup>
            <MChoiceItem v-model="selectedClient.hdrBrightnessMode" :title="$t('pin.hdr_brightness')" :cancel-label="$t('_common.cancel')" :options="[{ value: 'auto', label: $t('pin.hdr_brightness_auto') }, { value: 'manual', label: $t('pin.hdr_brightness_manual') }]" />
            <MFieldItem
              v-for="field in (selectedClient.hdrBrightnessMode === 'manual' ? brightnessFields : [])"
              :key="field.key"
              v-model="selectedClient[field.key]"
              type="number"
              :label="$t(field.label)"
              :min="field.min"
              :max="field.max"
              :step="field.step"
              :error="brightnessErrors(selectedClient, brightnessFields).includes(field.key) ? `${field.min} – ${field.max}` : ''"
            />
            <MChoiceItem v-model="selectedClient.hdrProfile" :title="$t('pin.color_profile_override')" :cancel-label="$t('_common.cancel')" :disabled="!hasIccFileList" :options="profileOptions" />
            <MChoiceItem v-model="selectedClient.deviceSize" :title="$t('pin.device_size')" :cancel-label="$t('_common.cancel')" :options="sizeOptions" />
          </MGroup>
          <MFooter v-if="selectedClient.hdrBrightnessRuntime?.active">{{ $t('connections.current_brightness') }} {{ formatHdrNits(selectedClient.hdrBrightnessRuntime.maxNits) }} nits</MFooter>
          <MFooter>{{ $t('connections.display_hint') }}</MFooter>
        </MSection>

        <MButton variant="danger" icon="link_off" @click="requestUnpair(selectedClient)">{{ $t('connections.unpair_device') }}</MButton>
        <MFooter>ID · {{ selectedClient.uuid }}</MFooter>
        <p v-if="clientError" class="pin-pair__error" role="alert">{{ clientError }}</p>
      </div>
      <template #actions>
        <MButton variant="text" :disabled="saving" @click="closeDevice">{{ $t('_common.cancel') }}</MButton>
        <MButton class="pin-pill" :loading="saving" :disabled="!canSaveDevice(selectedClient, brightnessFields)" @click="saveDevice">{{ $t('_common.save') }}</MButton>
      </template>
    </MSheet>

    <ConfirmDialog
      :show="!!confirmAction"
      dialog-id="client-action"
      :title="confirmationTitle"
      title-icon="warning"
      tone="danger"
      :dismissible="!actionBusy"
      :close-label="$t('_common.close')"
      @close="confirmAction = null"
    >
      <p>{{ confirmationBody }}</p>
      <p v-if="confirmAction?.type === 'disable'" class="pin-pair__hint">{{ $t('connections.disable_note') }}</p>
      <p v-if="clientError" class="pin-pair__error" role="alert">{{ clientError }}</p>
      <template #actions>
        <MButton variant="text" :disabled="actionBusy" @click="confirmAction = null">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" :disabled="actionBusy" @click="performAction">{{ $t(confirmAction?.type === 'disable' ? 'connections.disable_disconnect' : 'connections.confirm_unpair') }}</MButton>
      </template>
    </ConfirmDialog>
    <MSnackbar v-model="noticeOpen" :message="notice" />
  </div>
</template>

<script setup>
import { computed, inject, onMounted, onUnmounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { MBanner, MButton, MChoiceItem, MEmpty, MField, MFieldItem, MFooter, MGroup, MIcon, MItem, MPage, MPinPad, MPromo, MScallop, MSection, MSeg, MSheet, MSkeleton, MSnackbar, MSwitch, usePageFilter, contextTexts } from 'foundation-ui/vue'
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import { formatHdrNits, usePin } from '../composables/usePin.js'
import { useQrPair } from '../composables/useQrPair.js'
import { deviceIconFor } from '../utils/homeData.js'
import { clientTexts, filterClients, groupClients, kindOptions } from '../utils/clientList.js'
import { PIN_LENGTH, brightnessErrors, canSaveDevice, canSubmitPairing, clientState } from '../utils/pairing.js'

const { t, locale } = useI18n()
const webShell = inject('webShell', null)
const {
  pairingDeviceName, clients, clientsAt, hdrProfileList, hasIccFileList, loading, saving, updating, clientError, listError,
  refreshClients, unpairAll, unpairSingle, saveClient, startEdit, cancelEdit, setClientEnabled, pairClient, loadConfig, loadColorProfiles,
} = usePin(t)
// the page opens on the list skeleton and stays on it until the first list has come back (before that `loading` is still false and the empty state would show for a moment)
const listed = ref(false)
const { qrDataUrl, qrPin, qrRemaining, qrLoading, qrError, qrPaired, qrActive, generateQrCode, cancelQrCode } = useQrPair()

const tx = computed(() => clientTexts(locale.value))
// 30 clients are not one column: grouped by the kind of device, filtered by kind and by name
const kind = ref('all')
const query = ref('')
// a client chosen in the search: the filter takes its name, the row pulses
usePageFilter({ page: 'pin', apply: (q) => { kind.value = 'all'; query.value = q }, rowSelector: (q) => `[data-row="${CSS.escape(q)}"]` })
const kindChoices = computed(() => kindOptions(clients.value, tx.value))
const shown = computed(() => filterClients(clients.value, kind.value, query.value))
const groups = computed(() => groupClients(shown.value))
watch(kindChoices, (options) => { if (!options.some((o) => o.value === kind.value)) kind.value = 'all' })

const showPairing = ref(false)
const pairMode = ref('pin')
const pinValue = ref('')
const pairingBusy = ref(false)
const pairReady = computed(() => canSubmitPairing(pinValue.value, pairingDeviceName.value))
const selectedUuid = ref(null)
const selectedClient = computed(() => clients.value.find((client) => client.uuid === selectedUuid.value))
const confirmAction = ref(null)
const actionBusy = ref(false)
const notice = ref('')
const noticeOpen = ref(false)
const say = (text) => { notice.value = text; noticeOpen.value = true }
let refreshTimer

const STATE_TEXT = { streaming: 'connections.streaming', disabled: 'connections.disabled', allowed: 'connections.allowed' }
const stateOf = clientState
// The second line says something the switch does not: that the client is streaming now, or the device size when one was chosen (a medium, tablet size too); a client without a chosen size is treated as medium by the host, so every row would read the same.
const DEFAULT_DEVICE_SIZE = 'medium'
const rowText = (client) => (stateOf(client) === 'streaming' ? t('connections.streaming') : client.deviceSize && (client.deviceSizeSet || client.deviceSize !== DEFAULT_DEVICE_SIZE) ? `${t('pin.device_size')} · ${t(`pin.device_size_${client.deviceSize}`)}` : '')
const leadTone = (client) => ({ 'm-lead-circle--success': stateOf(client) === 'streaming', 'm-lead-circle--off': stateOf(client) === 'disabled' })

const brightnessFields = [
  { key: 'hdrBrightnessMaxNits', label: 'pin.hdr_brightness_peak', min: 1, max: 10000, step: 1 },
  { key: 'hdrBrightnessMinNits', label: 'pin.hdr_brightness_min', min: 0, max: 100, step: 0.001 },
  { key: 'hdrBrightnessMaxFullFrameNits', label: 'pin.hdr_brightness_full_frame', min: 1, max: 10000, step: 1 },
]
const sizeOptions = computed(() => ['small', 'medium', 'large', 'huge'].map((size) => ({ value: size, label: t(`pin.device_size_${size}`) })))
const profileOptions = computed(() => {
  const current = selectedClient.value?.hdrProfile
  const list = [{ value: '', label: t('pin.system_color_profile') }]
  if (current && !hdrProfileList.value.includes(current)) list.push({ value: current, label: current })
  return list.concat(hdrProfileList.value.map((profile) => ({ value: profile, label: profile })))
})
const confirmationTitle = computed(() => t(confirmAction.value?.type === 'disable' ? 'connections.disable_title' : 'connections.unpair_title'))
const confirmationBody = computed(() =>
  t(
    confirmAction.value?.type === 'disable' ? 'connections.disable_body' : confirmAction.value?.type === 'unpair-all' ? 'pin.unpair_all_confirm' : 'connections.unpair_body',
    { name: confirmAction.value?.client?.name },
  ),
)

// ---- Pairing ----
const openPairing = (mode = 'pin') => {
  clientError.value = ''
  pinValue.value = ''
  pairMode.value = mode === 'qr' ? 'qr' : 'pin'
  showPairing.value = true
  if (pairMode.value === 'qr' && !qrActive.value) void generateQrCode()
}
const closePairing = async () => {
  showPairing.value = false
  pinValue.value = ''
  if (qrActive.value) await cancelQrCode()
}
const setPairMode = async (mode) => {
  if (mode === 'pin' && qrActive.value) await cancelQrCode()
  pairMode.value = mode
}
const submitPairing = async () => {
  if (!pairReady.value || pairingBusy.value) return
  pairingBusy.value = true
  try {
    if (await pairClient(pinValue.value, pairingDeviceName.value.trim())) {
      await closePairing()
      say(t('connections.pair_sent'))
      await refreshClients()
    } else pinValue.value = ''
  } finally {
    pairingBusy.value = false
  }
}

// ---- Device settings ----
const editClient = (client) => {
  clientError.value = ''
  startEdit(client.uuid)
  selectedUuid.value = client.uuid
}
const closeDevice = () => {
  if (selectedUuid.value) cancelEdit(selectedUuid.value)
  selectedUuid.value = null
}
const saveDevice = async () => {
  if (!canSaveDevice(selectedClient.value, brightnessFields)) return
  selectedClient.value.name = selectedClient.value.name.trim()
  if (await saveClient(selectedUuid.value)) {
    selectedUuid.value = null
    say(t('connections.device_saved'))
  } else clientError.value ||= t('pin.save_failed')
}
const requestToggle = async (client, enabled) => {
  clientError.value = ''
  if (!enabled && client.streaming) {
    confirmAction.value = { type: 'disable', client }
    return
  }
  if (await setClientEnabled(client.uuid, enabled)) say(t('connections.permission_saved'))
}
const requestUnpair = (client) => {
  closeDevice()
  clientError.value = ''
  confirmAction.value = { type: 'unpair', client }
}
const performAction = async () => {
  if (!confirmAction.value) return
  actionBusy.value = true
  try {
    const action = confirmAction.value
    let success = false
    if (action.type === 'disable') success = await setClientEnabled(action.client.uuid, false)
    else if (action.type === 'unpair') success = await unpairSingle(action.client.uuid)
    else success = await unpairAll()
    if (success) {
      confirmAction.value = null
      say(t('connections.action_done'))
      if (!clients.value.length) openPairing()
    } else clientError.value ||= t('connections.action_failed')
  } finally {
    actionBusy.value = false
  }
}

// Right click on a client: settings, rename, allow or block, unpair
function onRowMenu(event, client) {
  if (!webShell?.showMenu) return
  const enabled = client.enabled !== false
  webShell.showMenu(event, [
    { id: 'settings', icon: 'settings', label: t('connections.device_settings') },
    { id: 'rename', icon: 'edit', label: t('pin.device_name') },
    { id: 'toggle', icon: enabled ? 'cancel' : 'check_circle', label: t(enabled ? 'connections.blocked' : 'connections.allowed') },
    ...(client.uuid ? [{ id: 'copy-id', icon: 'content_copy', label: contextTexts(String(locale.value)).copyDeviceId }] : []),
    { id: 'sep', divider: true },
    { id: 'unpair', icon: 'link_off', label: t('connections.unpair_device'), danger: true },
  ], client, (entry) => {
    if (entry.id === 'settings' || entry.id === 'rename') editClient(client)
    else if (entry.id === 'toggle') requestToggle(client, !enabled)
    else if (entry.id === 'copy-id') navigator.clipboard?.writeText(client.uuid).then(() => say(contextTexts(String(locale.value)).copied)).catch(() => {})
    else if (entry.id === 'unpair') requestUnpair(client)
  })
}
// Arrow keys move between rows on the same kind of control (switch to switch, gear to gear)
function rowKeys(event) {
  if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return
  const kind = event.target.closest?.('.pin-row__switch') ? '.pin-row__switch' : event.target.closest?.('.m-split-item__gear') ? '.m-split-item__gear' : ''
  if (!kind) return
  const all = [...event.currentTarget.querySelectorAll(kind)]
  const next = all[all.indexOf(event.target.closest(kind)) + (event.key === 'ArrowDown' ? 1 : -1)]
  if (next) { next.focus(); event.preventDefault() }
}

// "Pair a client" of the list pane's action button
watch(() => webShell?.pending.value, (request) => {
  if (request !== 'pair' && request !== 'pair-qr') return
  webShell.pending.value = ''
  openPairing(request === 'pair-qr' ? 'qr' : 'pin')
}, { immediate: true })
watch(qrPaired, async (paired) => {
  if (paired) {
    await closePairing()
    await refreshClients()
    say(t('pin.qr_paired_success'))
  }
})
onMounted(async () => {
  await loadConfig()
  const clientsLoaded = await refreshClients()
  listed.value = true
  await loadColorProfiles()
  if ((clientsLoaded && !clients.value.length) || window.location.hash === '#pairing') openPairing()
  else if (window.location.hash === '#pairing-qr') openPairing('qr')
  refreshTimer = setInterval(() => {
    if (!selectedUuid.value && !showPairing.value && !confirmAction.value && !loading.value && !document.hidden && !updating.value.size) void refreshClients()
  }, 5000)
})
onUnmounted(() => clearInterval(refreshTimer))
const SHAPES = { mobile: 'cookie', tablet: 'clover', tv: 'sunny', laptop_mac: 'squircle', desktop_mac: 'squircle', desktop_windows: 'squircle', sports_esports: 'gem', view_in_ar: 'burst' }
const shapeFor = (client) => SHAPES[deviceIconFor(client.name, client.os)] || 'circle'
</script>
