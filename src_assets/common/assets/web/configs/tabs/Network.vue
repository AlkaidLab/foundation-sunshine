<script setup>
import { computed, ref } from 'vue'
import WebhookCard from '../webhook/WebhookCard.vue'

const props = defineProps(['config'])

const defaultMoonlightPort = 47989

const config = ref(props.config)
const effectivePort = computed(() => +config.value?.port ?? defaultMoonlightPort)
</script>

<template>
  <div id="network" class="config-page">
    <!-- UPnP -->
    <div class="mb-3">
      <label for="upnp" class="form-label">{{ $t('config.upnp') }}</label>
      <select id="upnp" class="form-select" v-model="config.upnp">
        <option value="disabled">{{ $t('_common.disabled_def') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.upnp_desc') }}</div>
    </div>

    <!-- Address family -->
    <div class="mb-3">
      <label for="address_family" class="form-label">{{ $t('config.address_family') }}</label>
      <select id="address_family" class="form-select" v-model="config.address_family">
        <option value="ipv4">{{ $t('config.address_family_ipv4') }}</option>
        <option value="both">{{ $t('config.address_family_both') }}</option>
      </select>
      <div class="form-text">{{ $t('config.address_family_desc') }}</div>
    </div>

    <!-- Bind address -->
    <div class="mb-3">
      <label for="bind_address" class="form-label">{{ $t('config.bind_address') }}</label>
      <input type="text" class="form-control" id="bind_address" v-model="config.bind_address" />
      <div class="form-text">{{ $t('config.bind_address_desc') }}</div>
    </div>

    <!-- Port family -->
    <div class="mb-3">
      <label for="port" class="form-label">{{ $t('config.port') }}</label>
      <input type="number" min="1029" max="65514" class="form-control" id="port" :placeholder="defaultMoonlightPort"
             v-model="config.port" />
      <div class="form-text">{{ $t('config.port_desc') }}</div>
      <div class="alert alert-danger" v-if="(+effectivePort - 5) < 1024">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_1') }}
      </div>
      <div class="alert alert-danger" v-if="(+effectivePort + 21) > 65535">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_2') }}
      </div>
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
          <td>{{+effectivePort - 5}}</td>
          <td></td>
        </tr>
        <tr>
          <!-- HTTP -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort}}</td>
          <td>
            <div class="alert alert-primary" role="alert" v-if="+effectivePort !== defaultMoonlightPort">
              <i class="fa-solid fa-xl fa-circle-info"></i> {{ $t('config.port_http_port_note') }}
            </div>
          </td>
        </tr>
        <tr>
          <!-- Web UI -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort + 1}}</td>
          <td>{{ $t('config.port_web_ui') }}</td>
        </tr>
        <tr>
          <!-- RTSP -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort + 21}}</td>
          <td></td>
        </tr>
        <tr>
          <!-- Video, Control, Audio, Mic -->
          <td>{{ $t('config.port_udp') }}</td>
          <td>{{+effectivePort + 9}} - {{+effectivePort + 12}}</td>
          <td></td>
        </tr>
        </tbody>
      </table>
      </div>
      <div class="alert alert-warning" v-if="config.origin_web_ui_allowed === 'wan'">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_warning') }}
      </div>
    </div>

    <!-- Origin Web UI Allowed -->
    <div class="mb-3">
      <label for="origin_web_ui_allowed" class="form-label">{{ $t('config.origin_web_ui_allowed') }}</label>
      <select id="origin_web_ui_allowed" class="form-select" v-model="config.origin_web_ui_allowed">
        <option value="pc">{{ $t('config.origin_web_ui_allowed_pc') }}</option>
        <option value="lan">{{ $t('config.origin_web_ui_allowed_lan') }}</option>
        <option value="wan">{{ $t('config.origin_web_ui_allowed_wan') }}</option>
      </select>
      <div class="form-text">{{ $t('config.origin_web_ui_allowed_desc') }}</div>
    </div>

    <!-- External IP -->
    <div class="mb-3">
      <label for="external_ip" class="form-label">{{ $t('config.external_ip') }}</label>
      <input type="text" class="form-control" id="external_ip" placeholder="123.456.789.12" v-model="config.external_ip" />
      <div class="form-text">{{ $t('config.external_ip_desc') }}</div>
    </div>

    <!-- LAN Encryption Mode -->
    <div class="mb-3">
      <label for="lan_encryption_mode" class="form-label">{{ $t('config.lan_encryption_mode') }}</label>
      <select id="lan_encryption_mode" class="form-select" v-model="config.lan_encryption_mode">
        <option value="0">{{ $t('_common.disabled_def') }}</option>
        <option value="1">{{ $t('config.lan_encryption_mode_1') }}</option>
        <option value="2">{{ $t('config.lan_encryption_mode_2') }}</option>
      </select>
      <div class="form-text">{{ $t('config.lan_encryption_mode_desc') }}</div>
    </div>

    <!-- WAN Encryption Mode -->
    <div class="mb-3">
      <label for="wan_encryption_mode" class="form-label">{{ $t('config.wan_encryption_mode') }}</label>
      <select id="wan_encryption_mode" class="form-select" v-model="config.wan_encryption_mode">
        <option value="0">{{ $t('_common.disabled') }}</option>
        <option value="1">{{ $t('config.wan_encryption_mode_1') }}</option>
        <option value="2">{{ $t('config.wan_encryption_mode_2') }}</option>
      </select>
      <div class="form-text">{{ $t('config.wan_encryption_mode_desc') }}</div>
    </div>

    <!-- CLOSE VERIFY SAFE -->
    <div class="mb-3">
      <label for="close_verify_safe" class="form-label">{{ $t('config.close_verify_safe') }}</label>
      <select id="close_verify_safe" class="form-select" v-model="config.close_verify_safe">
        <option value="disabled">{{ $t('_common.disabled_def') }}</option>
        <option value="enabled">{{ $t('_common.enabled') }}</option>
      </select>
      <div class="form-text">{{ $t('config.close_verify_safe_desc') }}</div>
    </div>

    <!-- MDNS BROADCAST -->
    <div class="mb-3">
      <label for="mdns_broadcast" class="form-label">{{ $t('config.mdns_broadcast') }}</label>
      <select id="mdns_broadcast" class="form-select" v-model="config.mdns_broadcast">
        <option value="disabled">{{ $t('_common.disabled') }}</option>
        <option value="enabled">{{ $t('_common.enabled_def') }}</option>
      </select>
      <div class="form-text">{{ $t('config.mdns_broadcast_desc') }}</div>
    </div>

    <!-- Ping Timeout -->
    <div class="mb-3">
      <label for="ping_timeout" class="form-label">{{ $t('config.ping_timeout') }}</label>
      <input type="text" class="form-control" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
      <div class="form-text">{{ $t('config.ping_timeout_desc') }}</div>
    </div>

    <!-- Pair Max Attempts (per 60s window, per IP) -->
    <div class="mb-3">
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

    <!-- Webhook is stored and applied independently from the general configuration form. -->
    <WebhookCard />

  </div>
</template>

<style scoped>
.port-table-shell {
  margin-top: 1rem;
  overflow-x: auto;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

.port-table {
  min-width: 540px;
  margin: 0;
  color: var(--ui-text-secondary);
  --bs-table-bg: transparent;
  --bs-table-color: var(--ui-text-secondary);
  --bs-table-border-color: var(--ui-border);
  --bs-table-hover-bg: var(--ui-surface-hover);
  --bs-table-hover-color: var(--ui-text-primary);
}

.port-table thead th {
  padding: 0.8rem 1rem;
  border-bottom-color: var(--ui-border);
  background: var(--ui-surface-strong);
  color: var(--ui-text-primary);
  font-weight: 600;
}

.port-table td {
  padding: 0.75rem 1rem;
  vertical-align: middle;
}

.port-table .alert {
  margin: 0;
  padding: 0.5rem 0.75rem;
}
</style>
