<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { apiJson } from '../../utils/apiFetch.js'
import WebhookCard from '../webhook/WebhookCard.vue'

const props = defineProps(['config'])
const defaultMoonlightPort = 47989
const config = ref(props.config)
const networkPage = ref(null)
const listenerInfo = ref(null)
const listenerError = ref('')
const manualAddress = ref(false)
let listenerRequest = 0
const knownAddresses = computed(() => listenerInfo.value?.addresses || [])
const addressChoice = computed({
  get: () => {
    const address = config.value.bind_address || ''
    return manualAddress.value || (address && !knownAddresses.value.some((item) => item.address === address))
      ? '__manual__'
      : address
  },
  set: (value) => {
    manualAddress.value = value === '__manual__'
    if (!manualAddress.value) config.value.bind_address = value
  },
})
const activeListener = computed(() => listenerInfo.value?.active)
const savedListener = computed(() => listenerInfo.value?.saved)
const listenerFamilyLabel = (settings) => {
  if (settings.bind_address) return settings.bind_address.includes(':') ? 'IPv6' : 'IPv4'
  return settings.address_family === 'both' ? 'IPv4 + IPv6' : 'IPv4'
}
const listenerPending = computed(
  () =>
    activeListener.value &&
    savedListener.value &&
    ['bind_address', 'address_family', 'port', 'upnp'].some(
      (key) => String(activeListener.value[key]) !== String(savedListener.value[key]),
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

// Preserve direct links to settings inside collapsed address/port sections.
const revealLinkedSetting = () => {
  let target = document.getElementById(window.location.hash.slice(1))
  if (!networkPage.value?.contains(target)) return
  while (target && target !== networkPage.value) {
    if (target.tagName === 'DETAILS') target.open = true
    target = target.parentElement
  }
}
onMounted(() => {
  void loadListeners()
  revealLinkedSetting()
  window.addEventListener('hashchange', revealLinkedSetting)
  window.addEventListener('sunshine:config-saved', loadListeners)
})
onBeforeUnmount(() => {
  listenerRequest++
  window.removeEventListener('hashchange', revealLinkedSetting)
  window.removeEventListener('sunshine:config-saved', loadListeners)
})
</script>

<template>
  <div id="network" ref="networkPage" class="config-page network-layout">
    <div v-if="listenerPending" class="alert alert-warning mb-0" role="status">
      {{ $t('connections.pending_restart') }}
    </div>

    <section class="network-section" aria-labelledby="network-connect-title">
      <header class="network-section-header">
        <span class="network-section-icon" aria-hidden="true"><i class="fa-solid fa-wifi"></i></span>
        <div>
          <h2 id="network-connect-title">{{ $t('config.network_connect_title') }}</h2>
          <p>{{ $t('config.network_connect_desc') }}</p>
        </div>
      </header>
      <div class="network-settings-grid">
        <div class="network-field">
          <label for="mdns_broadcast" class="form-label">{{ $t('config.mdns_broadcast') }}</label>
          <select id="mdns_broadcast" class="form-select" v-model="config.mdns_broadcast">
            <option value="disabled">{{ $t('_common.disabled') }}</option>
            <option value="enabled">{{ $t('_common.enabled_def') }}</option>
          </select>
          <div class="form-text">{{ $t('config.mdns_broadcast_desc') }}</div>
        </div>
        <div class="network-field">
          <label for="upnp" class="form-label">{{ $t('config.upnp') }}</label>
          <select id="upnp" class="form-select" v-model="config.upnp">
            <option value="disabled">{{ $t('_common.disabled_def') }}</option>
            <option value="enabled">{{ $t('_common.enabled') }}</option>
          </select>
          <div class="form-text">{{ $t('config.upnp_desc') }}</div>
        </div>
      </div>
      <details class="network-advanced" open>
        <summary>{{ $t('config.network_manual_title') }}</summary>
        <p class="network-advanced-description">{{ $t('config.network_manual_desc') }}</p>
        <div v-if="activeListener" class="listener-effective mb-3">
          <span class="badge text-bg-success">{{ $t('connections.active') }}</span
          ><code
            >{{ activeListener.bind_address || $t('connections.all_addresses') }} ·
            {{ listenerFamilyLabel(activeListener) }} · {{ activeListener.port }}</code
          >
        </div>
        <p v-if="listenerError" class="text-warning small" role="status">
          {{ $t('connections.address_list_failed') }} {{ listenerError }}
        </p>
        <div class="network-settings-grid">
          <div class="network-field">
            <label for="address_family" class="form-label">{{ $t('config.address_family') }}</label>
            <select id="address_family" class="form-select" v-model="config.address_family">
              <option value="ipv4">{{ $t('config.address_family_ipv4') }}</option>
              <option value="both">{{ $t('config.address_family_both') }}</option>
            </select>
            <div class="form-text">{{ $t('config.address_family_desc') }}</div>
          </div>
          <div class="network-field">
            <label for="bind_address" class="form-label">{{ $t('config.bind_address') }}</label>
            <select id="bind_address" v-model="addressChoice" class="form-select">
              <option value="">{{ $t('connections.all_addresses') }}</option>
              <option v-for="entry in knownAddresses" :key="`${entry.name}-${entry.address}`" :value="entry.address">
                {{ entry.name }} · {{ entry.address }}
              </option>
              <option value="__manual__">{{ $t('connections.manual_address') }}</option>
            </select>
            <label v-if="addressChoice === '__manual__'" class="d-block mt-2"
              ><span class="visually-hidden">{{ $t('connections.local_ip') }}</span
              ><input
                v-model.trim="config.bind_address"
                type="text"
                class="form-control"
                :placeholder="$t('connections.local_ip')"
            /></label>
            <div class="form-text">{{ $t('connections.bind_help') }}</div>
            <div class="form-text">{{ $t('config.bind_address_desc') }}</div>
          </div>
          <div class="network-field">
            <label for="external_ip" class="form-label">{{ $t('config.external_ip') }}</label>
            <input
              type="text"
              class="form-control"
              id="external_ip"
              placeholder="123.456.789.12"
              v-model="config.external_ip"
            />
            <div class="form-text">{{ $t('config.external_ip_desc') }}</div>
          </div>
          <div class="network-field">
            <label for="port" class="form-label">{{ $t('config.port') }}</label>
            <input
              type="number"
              min="1029"
              max="65514"
              class="form-control"
              id="port"
              :placeholder="defaultMoonlightPort"
              v-model.number="config.port"
            />
            <div class="form-text">{{ $t('config.port_desc') }}</div>
            <div class="form-text" v-if="effectivePort !== defaultMoonlightPort">
              {{ $t('config.port_http_port_note') }}
            </div>
            <div class="alert alert-danger" v-if="+effectivePort - 5 < 1024">
              <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_1') }}
            </div>
            <div class="alert alert-danger" v-if="+effectivePort + 21 > 65535">
              <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_2') }}
            </div>
          </div>
        </div>
        <div class="listener-preview mt-3">
          <span>{{ $t('connections.proposed') }}</span
          ><code
            >{{ config.bind_address || $t('connections.all_addresses') }} ·
            {{ listenerFamilyLabel(config) }} · {{ effectivePort }}</code
          ><small>{{ $t('connections.source_note') }}</small>
        </div>
        <details class="network-port-reference">
          <summary>{{ $t('config.network_port_details') }}</summary>
          <div class="port-table-shell">
            <table class="table port-table">
              <thead>
                <tr>
                  <th scope="col">{{ $t('config.port_protocol') }}</th>
                  <th scope="col">{{ $t('config.port_port') }}</th>
                  <th scope="col">{{ $t('config.port_note') }}</th>
                </tr>
              </thead>
              <tbody>
                <tr>
                  <!-- HTTPS -->
                  <td>{{ $t('config.port_tcp') }}</td>
                  <td>{{ +effectivePort - 5 }}</td>
                  <td>HTTPS API</td>
                </tr>
                <tr>
                  <!-- HTTP -->
                  <td>{{ $t('config.port_tcp') }}</td>
                  <td>{{ +effectivePort }}</td>
                  <td>HTTP API</td>
                </tr>
                <tr>
                  <!-- Web UI -->
                  <td>{{ $t('config.port_tcp') }}</td>
                  <td>{{ +effectivePort + 1 }}</td>
                  <td>
                    <div>{{ $t('config.port_web_ui') }}</div>
                  </td>
                </tr>
                <tr>
                  <!-- RTSP -->
                  <td>{{ $t('config.port_tcp') }}</td>
                  <td>{{ +effectivePort + 21 }}</td>
                  <td>RTSP</td>
                </tr>
                <tr>
                  <!-- Video, Control, Audio, Mic -->
                  <td>{{ $t('config.port_udp') }}</td>
                  <td>{{ +effectivePort + 9 }} - {{ +effectivePort + 12 }}</td>
                  <td>{{ $t('config.network_stream_ports') }}</td>
                </tr>
              </tbody>
            </table>
          </div>
          <p class="form-text">{{ $t('config.port_web_ui_proxy_warning') }}</p>
        </details>
      </details>
    </section>

    <details class="network-section" aria-labelledby="network-streaming-title">
      <summary class="network-section-header">
        <span class="network-section-icon" aria-hidden="true"><i class="fa-solid fa-signal"></i></span>
        <div>
          <h2 id="network-streaming-title">{{ $t('config.network_streaming_title') }}</h2>
          <p>{{ $t('config.network_streaming_desc') }}</p>
        </div>
      </summary>
      <div class="network-settings-grid">
        <div class="network-field network-field-wide">
          <label for="fec_auto" class="form-label">{{ $t('config.fec_auto') }}</label>
          <select id="fec_auto" class="form-select" v-model="config.fec_auto">
            <option value="disabled">{{ $t('_common.disabled_def') }}</option>
            <option value="enabled">{{ $t('_common.enabled') }}</option>
          </select>
          <div class="form-text">{{ $t('config.fec_auto_desc') }}</div>
        </div>

        <div class="network-field">
          <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
          <input
            id="fec_percentage"
            v-model.number="config.fec_percentage"
            class="form-control"
            type="number"
            min="0"
            max="255"
            step="1"
            placeholder="20"
          />
          <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
        </div>

        <div class="network-field">
          <label for="fec_auto_max_percentage" class="form-label">{{ $t('config.fec_auto_max_percentage') }}</label>
          <input
            id="fec_auto_max_percentage"
            v-model.number="config.fec_auto_max_percentage"
            class="form-control"
            type="number"
            min="0"
            max="100"
            step="1"
            placeholder="50"
          />
          <div class="form-text">{{ $t('config.fec_auto_max_percentage_desc') }}</div>
        </div>
      </div>
      <p class="network-section-note">{{ $t('config.fec_apply_note') }}</p>
    </details>

    <details class="network-section" aria-labelledby="network-security-title">
      <summary class="network-section-header">
        <span class="network-section-icon" aria-hidden="true"><i class="fa-solid fa-lock"></i></span>
        <div>
          <h2 id="network-security-title">{{ $t('config.network_security_title') }}</h2>
          <p>{{ $t('config.network_security_desc') }}</p>
        </div>
      </summary>
      <div class="network-field">
        <label for="origin_web_ui_allowed" class="form-label">{{ $t('config.origin_web_ui_allowed') }}</label>
        <select id="origin_web_ui_allowed" class="form-select" v-model="config.origin_web_ui_allowed">
          <option value="pc">{{ $t('config.origin_web_ui_allowed_pc') }}</option>
          <option value="lan">{{ $t('config.origin_web_ui_allowed_lan') }}</option>
          <option value="wan">{{ $t('config.origin_web_ui_allowed_wan') }}</option>
        </select>
        <div class="form-text">{{ $t('config.origin_web_ui_allowed_desc') }}</div>
      </div>
      <p class="network-section-note">{{ $t('config.network_web_access_note') }}</p>
      <div class="alert alert-warning" role="alert" v-if="config.origin_web_ui_allowed === 'wan'">
        <i class="fa-solid fa-triangle-exclamation me-1" aria-hidden="true"></i>
        {{ $t('config.port_warning') }}
      </div>
      <div class="network-settings-grid network-separated-fields">
        <div class="network-field">
          <label for="lan_encryption_mode" class="form-label">{{ $t('config.lan_encryption_mode') }}</label>
          <select id="lan_encryption_mode" class="form-select" v-model="config.lan_encryption_mode">
            <option value="0">{{ $t('_common.disabled_def') }}</option>
            <option value="1">{{ $t('config.lan_encryption_mode_1') }}</option>
            <option value="2">{{ $t('config.lan_encryption_mode_2') }}</option>
          </select>
          <div class="form-text">{{ $t('config.lan_encryption_mode_desc') }}</div>
        </div>
        <div class="network-field">
          <label for="wan_encryption_mode" class="form-label">{{ $t('config.wan_encryption_mode') }}</label>
          <select id="wan_encryption_mode" class="form-select" v-model="config.wan_encryption_mode">
            <option value="0">{{ $t('_common.disabled') }}</option>
            <option value="1">{{ $t('config.wan_encryption_mode_1') }}</option>
            <option value="2">{{ $t('config.wan_encryption_mode_2') }}</option>
          </select>
          <div class="form-text">{{ $t('config.wan_encryption_mode_desc') }}</div>
        </div>
      </div>
    </details>

    <details class="network-section" aria-labelledby="network-pairing-title">
      <summary class="network-section-header">
        <span class="network-section-icon" aria-hidden="true"><i class="fa-solid fa-link"></i></span>
        <div>
          <h2 id="network-pairing-title">{{ $t('config.network_pairing_title') }}</h2>
          <p>{{ $t('config.network_pairing_desc') }}</p>
        </div>
      </summary>
      <a href="/pin#client-list" class="btn btn-link px-0 mb-3"
        >{{ $t('connections.manage_clients') }} <i class="fas fa-arrow-up-right-from-square ms-1"
      /></a>
      <div class="network-settings-grid">
        <div class="network-field">
          <label for="pair_max_attempts" class="form-label">{{ $t('config.pair_max_attempts') }}</label>
          <input
            type="number"
            class="form-control"
            id="pair_max_attempts"
            min="0"
            max="50"
            step="1"
            placeholder="10"
            v-model.number="config.pair_max_attempts"
          />
          <div class="form-text">{{ $t('config.pair_max_attempts_desc') }}</div>
        </div>
        <div class="network-field">
          <label for="ping_timeout" class="form-label">{{ $t('config.ping_timeout') }}</label>
          <input type="text" class="form-control" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
          <div class="form-text">{{ $t('config.ping_timeout_desc') }}</div>
        </div>
        <div class="network-field network-field-wide">
          <label for="close_verify_safe" class="form-label">{{ $t('config.close_verify_safe') }}</label>
          <select id="close_verify_safe" class="form-select" v-model="config.close_verify_safe">
            <option value="disabled">{{ $t('_common.disabled_def') }}</option>
            <option value="enabled">{{ $t('_common.enabled') }}</option>
          </select>
          <div class="form-text">{{ $t('config.close_verify_safe_desc') }}</div>
        </div>
      </div>
    </details>

    <WebhookCard />
  </div>
</template>

<style scoped>
.listener-effective {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 0.65rem;
}
.listener-effective code,
.listener-preview code {
  color: color-mix(in srgb, var(--ui-text-primary) 33%, var(--ui-accent-text));
}
.form-control:focus-visible,
.form-select:focus-visible {
  outline: 2px solid var(--ui-accent-text);
  outline-offset: 3px;
}
.listener-preview {
  padding: 1rem;
  background: var(--ui-accent-soft);
  border-radius: var(--ui-radius-sm);
  font-size: 0.85rem;
}
.listener-preview code,
.listener-preview small {
  display: block;
  margin-top: 0.35rem;
  overflow-wrap: anywhere;
}
details.network-section > summary {
  cursor: pointer;
  margin-bottom: 0;
}
details.network-section[open] > summary {
  margin-bottom: 1.5rem;
}
details.network-section > summary::after {
  content: '+';
  margin-left: auto;
  color: var(--ui-accent);
}
details.network-section[open] > summary::after {
  content: '−';
}
.network-layout {
  --network-text-secondary: color-mix(in srgb, var(--ui-text-primary) 33%, var(--ui-text-secondary));
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 1.25rem;
}
.network-section {
  padding: clamp(1rem, 2vw, 1.5rem);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}
.network-section-header {
  display: flex;
  align-items: flex-start;
  gap: 0.875rem;
  margin-bottom: 1.5rem;
}
.network-section-icon {
  display: grid;
  place-items: center;
  flex: 0 0 2.5rem;
  width: 2.5rem;
  height: 2.5rem;
  border: 1px solid var(--ui-border);
  border-radius: 0.75rem;
  color: var(--ui-text-primary);
  background: var(--ui-surface-strong);
}
.network-section-header h2 {
  margin: 0 0 0.375rem;
  font-size: 1.125rem;
  font-weight: 600;
  color: var(--ui-text-primary);
}
.network-section-header p {
  margin: 0;
  color: var(--network-text-secondary);
  font-size: 0.875rem;
  line-height: 1.6;
}
.network-settings-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 1.5rem;
}
.network-field {
  min-width: 0;
}
.network-field-wide {
  grid-column: 1 / -1;
}
.network-field .form-label {
  color: var(--network-text-secondary);
  font-weight: 500;
}
.network-field .form-text {
  color: var(--network-text-secondary);
  margin-top: 0.5rem;
  line-height: 1.6;
}
.network-section-note {
  margin: 1rem 0 0;
  font-size: 0.875rem;
  line-height: 1.6;
  color: var(--network-text-secondary);
}
.network-separated-fields {
  margin-top: 1.5rem;
  padding-top: 1.5rem;
  border-top: 1px solid var(--ui-border);
}
.network-advanced {
  margin-top: 1.5rem;
  padding-top: 1rem;
  border-top: 1px solid var(--ui-border);
}
.network-advanced summary,
.network-port-reference summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 0.375rem 0;
  cursor: pointer;
  list-style: none;
  font-weight: 500;
  color: var(--ui-text-primary);
}
summary::-webkit-details-marker {
  display: none;
}
summary::after {
  content: '+';
  flex-shrink: 0;
  font-size: 1.25rem;
  line-height: 1;
}
details[open] > summary::after {
  content: '−';
}
summary:focus-visible {
  outline: 2px solid var(--ui-text-primary);
  outline-offset: 4px;
  border-radius: 0.25rem;
}
.network-advanced-description {
  margin: 0.5rem 0 1.25rem;
  color: var(--network-text-secondary);
  font-size: 0.875rem;
}
.network-port-reference {
  margin-top: 1.25rem;
}
.port-table-shell {
  margin-top: 0.75rem;
  overflow-x: auto;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
}
.port-table {
  min-width: 420px;
  margin: 0;
  color: var(--ui-text-secondary);
  --bs-table-bg: transparent;
  --bs-table-color: var(--ui-text-secondary);
  --bs-table-border-color: var(--ui-border);
}
.port-table thead th {
  padding: 0.75rem 1rem;
  background: var(--ui-surface-strong);
  color: var(--ui-text-primary);
  font-weight: 600;
}
.port-table td {
  padding: 0.75rem 1rem;
  vertical-align: middle;
}
.port-table th:nth-child(-n + 2),
.port-table td:nth-child(-n + 2) {
  white-space: nowrap;
}
@media (max-width: 767px) {
  .network-settings-grid {
    grid-template-columns: minmax(0, 1fr);
    gap: 1.25rem;
  }
  .network-section-header {
    gap: 0.75rem;
    margin-bottom: 1.25rem;
  }
}
</style>
