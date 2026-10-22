<template>
  <div class="pin-page">
    <Navbar />
    <main id="content" class="container py-4 client-page-content">
      <header class="client-page-header">
        <div>
          <h1 class="page-title">{{ $t('connections.title') }}</h1>
          <p class="page-subtitle">{{ $t('connections.description') }}</p>
        </div>
        <button class="btn btn-primary client-add-button" type="button" @click="openPairing">
          <i class="fas fa-plus me-2" />{{ $t('connections.add') }}
        </button>
      </header>
      <div v-if="clientError || listError" class="alert alert-danger mt-3" role="alert">{{ clientError || listError }}</div>
      <div v-if="notice" class="alert alert-success mt-3" role="status">{{ notice }}</div>
      <section id="client-list" class="card client-list" :aria-label="$t('connections.paired')">
        <header class="card-header client-list-heading">
          <h2 class="h5 mb-0">{{ $t('connections.paired') }}</h2>
          <span class="badge text-bg-secondary">{{ clients.length }}</span>
          <small class="text-muted">{{ $t('connections.immediate') }}</small>
        </header>
        <p v-if="loading && !clients.length" class="p-4 mb-0 text-muted" role="status">
          {{ $t('pin.loading_clients') }}
        </p>
        <div v-else-if="!clients.length" class="p-4 text-center text-muted">{{ $t('connections.empty') }}</div>
        <article v-for="client in clients" :key="client.uuid" class="client-row">
          <div class="client-avatar" aria-hidden="true">{{ clientInitial(client.name) }}</div>
          <div class="client-information">
            <div class="client-name">
              {{ client.name || $t('pin.unknown_client') }}
              <span
                v-if="!client.enabled || client.streaming"
                class="client-status"
                :class="{ 'client-status-live': client.enabled && client.streaming }"
              >{{
                $t(client.enabled && client.streaming ? 'connections.streaming' : 'connections.disabled')
              }}</span>
            </div>
            <div class="client-summary">
              {{
                $t(client.hdrBrightnessMode === 'manual' ? 'pin.hdr_brightness_manual' : 'pin.hdr_brightness_auto')
              }}
              · {{ client.hdrProfile || $t('pin.system_color_profile') }} ·
              {{ $t(`pin.device_size_${client.deviceSize}`) }}
            </div>
          </div>
          <div class="client-row-actions">
            <label class="form-check form-switch mb-0 d-flex align-items-center gap-2">
              <input
                class="form-check-input m-0"
                type="checkbox"
                role="switch"
                :checked="client.enabled"
                :disabled="updating.has(client.uuid) || loading || saving"
                :aria-label="$t('connections.allow_device', { name: client.name })"
                @change="requestToggle(client, $event)"
              />
              <span>{{ $t(client.enabled ? 'connections.allowed' : 'connections.blocked') }}</span>
            </label>
            <button
              class="btn btn-outline-secondary client-settings-button"
              type="button"
              :disabled="updating.has(client.uuid)"
              :aria-label="$t('connections.settings_for', { name: client.name })"
              @click="editClient(client)"
            >
              <i class="fas fa-sliders me-1" />{{ $t('connections.device_settings') }}
            </button>
          </div>
        </article>
        <p class="client-permission-note"><i class="fas fa-shield-halved me-2" />{{ $t('connections.disable_note') }}</p>
      </section>
      <aside class="card client-network-link">
        <div>
          <h2 class="h6 mb-1">{{ $t('connections.network_question') }}</h2>
          <p class="text-muted small mb-0">{{ $t('connections.network_hint') }}</p>
        </div>
        <a href="/config#bind_address" class="btn btn-link"
          >{{ $t('connections.network_link') }} <i class="fas fa-arrow-up-right-from-square ms-1"
        /></a>
      </aside>
      <details class="client-danger-zone">
        <summary>{{ $t('connections.danger_zone') }}</summary>
        <p class="text-muted small mt-3">{{ $t('pin.remove_paired_devices_desc') }}</p>
        <button
          class="btn btn-outline-danger btn-sm"
          type="button"
          :disabled="actionBusy || !clients.length"
          @click="confirmAction = { type: 'unpair-all' }"
        >
          {{ $t('troubleshooting.unpair_all') }}
        </button>
      </details>
    </main>

    <ConfirmDialog
      :show="showPairing"
      dialog-id="pair-client"
      :title="$t('connections.add')"
      title-icon="fas fa-link"
      max-width="520px"
      :dismissible="!pairingBusy && !qrLoading"
      :close-label="$t('_common.close')"
      @close="closePairing"
    >
      <div class="pair-mode-tabs" role="tablist" :aria-label="$t('connections.pair_method')">
        <button
          id="pair-pin-tab"
          type="button"
          role="tab"
          aria-controls="pair-pin-panel"
          :aria-selected="pairMode === 'pin'"
          class="btn"
          :class="pairMode === 'pin' ? 'btn-primary' : 'btn-outline-secondary'"
          @click="setPairMode('pin')"
        >
          {{ $t('pin.or_manual_pin') }}
        </button>
        <button
          id="pair-qr-tab"
          type="button"
          role="tab"
          aria-controls="pair-qr-panel"
          :aria-selected="pairMode === 'qr'"
          class="btn"
          :class="pairMode === 'qr' ? 'btn-primary' : 'btn-outline-secondary'"
          @click="setPairMode('qr')"
        >
          {{ $t('pin.qr_pairing') }}
        </button>
      </div>
      <form
        v-show="pairMode === 'pin'"
        id="pair-pin-panel"
        role="tabpanel"
        aria-labelledby="pair-pin-tab"
        @submit.prevent="submitPairing"
      >
        <p class="text-muted small">{{ $t('connections.pin_help') }}</p>
        <FormField id="pair-pin" label="PIN" required>
          <input
            id="pair-pin"
            v-model="pinValue"
            class="form-control"
            inputmode="numeric"
            pattern="[0-9]{4}"
            maxlength="4"
            autocomplete="off"
            required
          />
        </FormField>
        <FormField id="pair-name" :label="$t('pin.device_name')" required>
          <input id="pair-name" v-model="pairingDeviceName" class="form-control" maxlength="256" required />
        </FormField>
        <p v-if="clientError" class="text-danger small" role="alert">{{ clientError }}</p>
        <button class="btn btn-primary" :disabled="pairingBusy">{{ $t('pin.send') }}</button>
      </form>
      <div
        v-show="pairMode === 'qr'"
        id="pair-qr-panel"
        role="tabpanel"
        aria-labelledby="pair-qr-tab"
        class="text-center"
      >
        <p class="text-muted small">{{ $t('pin.qr_pairing_desc') }}</p>
        <div v-if="qrActive">
          <img :src="qrDataUrl" class="pair-qr-image" alt="QR Code" />
          <p class="mt-2 mb-1">
            PIN <strong class="fs-4 ms-2">{{ qrPin }}</strong>
          </p>
          <p class="text-muted small">{{ $t('pin.qr_expires_in', { seconds: qrRemaining }) }}</p>
        </div>
        <div v-if="qrError" class="alert alert-danger" role="alert">{{ qrError }}</div>
        <button class="btn btn-primary" :disabled="qrLoading" @click="generateQrCode">
          {{ $t(qrActive ? 'pin.qr_refresh' : 'pin.qr_generate') }}
        </button>
        <p class="alert alert-warning small mt-3 text-start">{{ $t('pin.qr_pairing_warning') }}</p>
      </div>
    </ConfirmDialog>

    <ConfirmDialog
      :show="!!selectedClient"
      dialog-id="device-settings"
      :title="$t('connections.device_settings')"
      title-icon="fas fa-sliders"
      max-width="540px"
      :dismissible="!saving"
      :close-label="$t('_common.close')"
      @close="closeDevice"
    >
      <form v-if="selectedClient" id="device-settings-form" @submit.prevent="saveDevice">
        <p class="text-muted small text-break">ID · {{ selectedClient.uuid }}</p>
        <FormField id="device-name" :label="$t('pin.device_name')" required>
          <input id="device-name" v-model="selectedClient.name" class="form-control" maxlength="256" required />
        </FormField>
        <h3 class="h6">{{ $t('connections.display_settings') }}</h3>
        <p class="text-muted small">{{ $t('connections.display_hint') }}</p>
        <FormField id="device-hdr" :label="$t('pin.hdr_brightness')">
          <select id="device-hdr" v-model="selectedClient.hdrBrightnessMode" class="form-select">
            <option value="auto">{{ $t('pin.hdr_brightness_auto') }}</option>
            <option value="manual">{{ $t('pin.hdr_brightness_manual') }}</option>
          </select>
        </FormField>
        <div v-if="selectedClient.hdrBrightnessMode === 'manual'" class="row g-3 mb-4">
          <FormField
            v-for="field in brightnessFields"
            :id="`device-${field.key}`"
            :key="field.key"
            :label="$t(field.label)"
            class="col-12 col-sm-6 mb-0"
            required
          >
            <input
              :id="`device-${field.key}`"
              v-model.number="selectedClient[field.key]"
              class="form-control"
              type="number"
              :min="field.min"
              :max="field.max"
              :step="field.step"
              required
            />
          </FormField>
        </div>
        <p v-if="selectedClient.hdrBrightnessRuntime?.active" class="text-muted small">
          {{ $t('connections.current_brightness') }}
          {{ formatHdrNits(selectedClient.hdrBrightnessRuntime.maxNits) }} nits
        </p>
        <FormField id="device-profile" :label="$t('pin.color_profile_override')">
          <select
            id="device-profile"
            v-model="selectedClient.hdrProfile"
            class="form-select"
            :disabled="!hasIccFileList"
          >
            <option value="">{{ $t('pin.system_color_profile') }}</option>
            <option
              v-if="selectedClient.hdrProfile && !hdrProfileList.includes(selectedClient.hdrProfile)"
              :value="selectedClient.hdrProfile"
            >
              {{ selectedClient.hdrProfile }}
            </option>
            <option v-for="profile in hdrProfileList" :key="profile" :value="profile">{{ profile }}</option>
          </select>
        </FormField>
        <FormField id="device-size" :label="$t('pin.device_size')">
          <select id="device-size" v-model="selectedClient.deviceSize" class="form-select">
            <option v-for="size in ['small', 'medium', 'large']" :key="size" :value="size">
              {{ $t(`pin.device_size_${size}`) }}
            </option>
          </select>
        </FormField>
        <button type="button" class="btn btn-outline-danger btn-sm" @click="requestUnpair(selectedClient)">
          {{ $t('connections.unpair_device') }}
        </button>
        <p v-if="clientError" class="text-danger small mt-3" role="alert">{{ clientError }}</p>
      </form>
      <template #actions
        ><button class="btn btn-secondary" :disabled="saving" @click="closeDevice">{{ $t('_common.cancel') }}</button
        ><button class="btn btn-primary" type="submit" form="device-settings-form" :disabled="saving">
          {{ $t('_common.save') }}
        </button></template
      >
    </ConfirmDialog>

    <ConfirmDialog
      :show="!!confirmAction"
      dialog-id="client-action"
      :title="confirmationTitle"
      title-icon="fas fa-triangle-exclamation"
      tone="danger"
      :dismissible="!actionBusy"
      :close-label="$t('_common.close')"
      @close="confirmAction = null"
    >
      <p>{{ confirmationBody }}</p>
      <p v-if="confirmAction?.type === 'disable'" class="text-muted small">{{ $t('connections.disable_note') }}</p>
      <p v-if="clientError" class="text-danger small" role="alert">{{ clientError }}</p>
      <template #actions
        ><button class="btn btn-secondary" :disabled="actionBusy" @click="confirmAction = null">
          {{ $t('_common.cancel') }}</button
        ><button class="btn btn-danger" :disabled="actionBusy" @click="performAction">
          {{ $t(confirmAction?.type === 'disable' ? 'connections.disable_disconnect' : 'connections.confirm_unpair') }}
        </button></template
      >
    </ConfirmDialog>
  </div>
</template>

<script setup>
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import Navbar from '../components/layout/Navbar.vue'
import ConfirmDialog from '../components/common/ConfirmDialog.vue'
import FormField from '../components/FormField.vue'
import { formatHdrNits, usePin } from '../composables/usePin.js'
import { useQrPair } from '../composables/useQrPair.js'

const { t } = useI18n()
const initialSegmenter = new Intl.Segmenter(undefined, { granularity: 'grapheme' })
const clientInitial = (name) => {
  const label = name?.trim() || t('pin.unknown_client')
  for (const { segment } of initialSegmenter.segment(label)) {
    if (/[\p{L}\p{N}]/u.test(segment)) return segment.toUpperCase()
  }
  return '?'
}
const {
  pairingDeviceName,
  clients,
  hdrProfileList,
  hasIccFileList,
  loading,
  saving,
  updating,
  clientError,
  listError,
  refreshClients,
  unpairAll,
  unpairSingle,
  saveClient,
  startEdit,
  cancelEdit,
  setClientEnabled,
  pairClient,
  loadConfig,
  loadColorProfiles,
} = usePin(t)
const { qrDataUrl, qrPin, qrRemaining, qrLoading, qrError, qrPaired, qrActive, generateQrCode, cancelQrCode } =
  useQrPair()
const showPairing = ref(false)
const pairMode = ref('pin')
const pinValue = ref('')
const pairingBusy = ref(false)
const selectedUuid = ref(null)
const selectedClient = computed(() => clients.value.find((client) => client.uuid === selectedUuid.value))
const confirmAction = ref(null)
const actionBusy = ref(false)
const notice = ref('')
let refreshTimer
const brightnessFields = [
  { key: 'hdrBrightnessMaxNits', label: 'pin.hdr_brightness_peak', min: 1, max: 10000, step: 1 },
  { key: 'hdrBrightnessMinNits', label: 'pin.hdr_brightness_min', min: 0, max: 100, step: 0.001 },
  { key: 'hdrBrightnessMaxFullFrameNits', label: 'pin.hdr_brightness_full_frame', min: 1, max: 10000, step: 1 },
]
const confirmationTitle = computed(() =>
  t(confirmAction.value?.type === 'disable' ? 'connections.disable_title' : 'connections.unpair_title'),
)
const confirmationBody = computed(() =>
  t(
    confirmAction.value?.type === 'disable'
      ? 'connections.disable_body'
      : confirmAction.value?.type === 'unpair-all'
        ? 'pin.unpair_all_confirm'
        : 'connections.unpair_body',
    { name: confirmAction.value?.client?.name },
  ),
)
const openPairing = () => {
  clientError.value = ''
  notice.value = ''
  showPairing.value = true
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
  if (!selectedClient.value?.name.trim()) return
  selectedClient.value.name = selectedClient.value.name.trim()
  if (await saveClient(selectedUuid.value)) {
    selectedUuid.value = null
    notice.value = t('connections.device_saved')
  } else clientError.value ||= t('pin.save_failed')
}
const submitPairing = async () => {
  pairingBusy.value = true
  try {
    if (await pairClient(pinValue.value, pairingDeviceName.value.trim())) {
      await closePairing()
      notice.value = t('connections.pair_sent')
      await refreshClients()
    }
  } finally {
    pairingBusy.value = false
  }
}
const requestToggle = async (client, event) => {
  const enabled = event.target.checked
  event.target.checked = client.enabled
  clientError.value = ''
  if (!enabled && client.streaming) {
    confirmAction.value = { type: 'disable', client }
    return
  }
  if (await setClientEnabled(client.uuid, enabled)) notice.value = t('connections.permission_saved')
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
      notice.value = t('connections.action_done')
      if (!clients.value.length) openPairing()
    } else clientError.value ||= t('connections.action_failed')
  } finally {
    actionBusy.value = false
  }
}
watch(qrPaired, async (paired) => {
  if (paired) {
    await closePairing()
    await refreshClients()
    notice.value = t('pin.qr_paired_success')
  }
})
onMounted(async () => {
  await loadConfig()
  const clientsLoaded = await refreshClients()
  await loadColorProfiles()
  if ((clientsLoaded && !clients.value.length) || window.location.hash === '#pairing') openPairing()
  refreshTimer = setInterval(() => {
    if (
      !selectedUuid.value &&
      !showPairing.value &&
      !confirmAction.value &&
      !loading.value &&
      !document.hidden &&
      !updating.value.size
    )
      void refreshClients()
  }, 5000)
})
onUnmounted(() => clearInterval(refreshTimer))
</script>

<style>
@import '../styles/global.less';
</style>
<style scoped lang="less">
.pin-page {
  min-height: 100vh;
  padding-bottom: var(--spacing-xl);
  color: var(--ui-text-primary);
}
.client-page-content {
  --client-text-secondary: color-mix(in srgb, var(--ui-text-primary) 33%, var(--ui-text-secondary));
  margin-top: 0.5rem;
}
.client-page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1.5rem;
  flex-wrap: wrap;
}
.client-page-header h1 {
  margin: 0 0 0.5rem;
}
.client-add-button {
  min-height: 2.75rem;
}
.client-list {
  margin-top: 2rem;
  overflow: hidden;
  backdrop-filter: blur(20px);
}
.client-list-heading {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 0.65rem;
  padding: 1.1rem 1.25rem;
}
.client-list-heading h2 {
  font-size: 0.95rem;
}
.client-list-heading small {
  margin-left: auto;
  font-size: 0.75rem;
}
.client-row {
  display: grid;
  grid-template-columns: 3rem minmax(0, 1fr) auto;
  gap: 1.1rem;
  align-items: center;
  padding: 1.4rem 1.25rem;
  transition: background 0.15s ease;
}
.client-row + .client-row {
  border-top: 1px solid var(--ui-border);
}
.client-row:hover,
.client-row:focus-within {
  background: color-mix(in srgb, var(--ui-surface-hover) 55%, transparent);
}
.client-avatar {
  display: grid;
  place-items: center;
  aspect-ratio: 1;
  border: 1px solid var(--ui-border);
  border-radius: 0.85rem;
  color: var(--ui-accent-text);
  background: var(--ui-accent-soft);
  font-size: 1.2rem;
  font-weight: 600;
  line-height: 1;
  user-select: none;
}
.client-name {
  display: flex;
  align-items: center;
  gap: 0.6rem;
  flex-wrap: wrap;
  font-weight: 600;
  overflow-wrap: anywhere;
}
.client-summary {
  color: var(--client-text-secondary);
  font-size: 0.8rem;
  margin-top: 0.4rem;
  overflow-wrap: anywhere;
}
.client-status {
  display: inline-flex;
  align-items: center;
  gap: 0.35rem;
  color: var(--client-text-secondary);
  font-size: 0.75rem;
  font-weight: 400;
}
.client-status::before {
  content: '';
  width: 0.35rem;
  height: 0.35rem;
  border-radius: 50%;
  background: currentColor;
  opacity: 0.6;
}
.client-status-live::before {
  background: var(--ui-success);
  opacity: 1;
}
.client-row-actions {
  display: flex;
  align-items: center;
  gap: 1.1rem;
}
.client-settings-button {
  min-height: 2.75rem;
  font-size: 0.8rem;
}
.client-row-actions .form-check {
  padding: 0;
  font-size: 0.8rem;
  white-space: nowrap;
  min-height: 2.75rem;
}
.client-permission-note {
  color: var(--client-text-secondary);
  font-size: 0.8rem;
  margin: 0;
  padding: 1rem 1.25rem;
  border-top: 1px solid var(--ui-border);
  line-height: 1.7;
}
.client-network-link {
  display: flex;
  flex-direction: row;
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
  gap: 1rem;
  padding: 1.25rem;
  margin-top: 1.5rem;
  backdrop-filter: blur(20px);
}
.client-danger-zone {
  margin-top: 1.5rem;
  padding-top: 1rem;
}
.client-danger-zone summary {
  color: var(--text-title-color, var(--ui-text-primary));
  font-size: 0.8rem;
  cursor: pointer;
}
.pair-mode-tabs {
  display: flex;
  gap: 0.75rem;
  margin-bottom: 1.5rem;
  flex-wrap: wrap;
}
.pair-mode-tabs button {
  flex: 1;
  min-height: 2.75rem;
}
.pair-qr-image {
  width: 280px;
  max-width: 100%;
  height: auto;
  border-radius: var(--ui-radius-sm);
}
@media (max-width: 991px) {
  .client-row {
    grid-template-columns: 2.5rem minmax(0, 1fr);
  }
  .client-row-actions {
    grid-column: 2;
    justify-content: space-between;
  }
}
@media (max-width: 575px) {
  .client-page-content {
    margin-top: 0;
  }
  .client-page-header h1 {
    font-size: 1.4rem;
  }
  .client-page-header p {
    font-size: 0.875rem;
    line-height: 1.7;
  }
  .client-row {
    padding: 1.1rem;
    gap: 0.75rem;
  }
  .client-row-actions {
    gap: 0.5rem;
    flex-wrap: wrap;
  }
  .client-list-heading small {
    flex-basis: 100%;
    margin-left: 0;
  }
  .client-network-link {
    gap: 0.5rem;
  }
  .client-network-link .btn-link {
    padding-left: 0;
  }
}
</style>
