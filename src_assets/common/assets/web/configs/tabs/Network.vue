<script setup>
import { computed, ref } from 'vue'

const props = defineProps([
  'platform',
  'config'
])

const defaultMoonlightPort = 47989

const config = ref(props.config)
const effectivePort = computed(() => +config.value?.port ?? defaultMoonlightPort)

// Webhook test function
const testWebhook = async () => {
  if (!config.value.webhook_url) {
    alert($t('config.webhook_test_url_required'))
    return
  }

  // Check if URL is valid
  try {
    new URL(config.value.webhook_url)
  } catch (error) {
    alert($t('config.webhook_test_failed') + ': Invalid URL format')
    return
  }

  try {
    // Create AbortController for timeout
    const controller = new AbortController()
    const timeout = parseInt(config.value.webhook_timeout) || 1000 // Use configured timeout, default 1000ms
    const timeoutId = setTimeout(() => controller.abort(), timeout)

    const response = await fetch(config.value.webhook_url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: '{"msgtype": "text", "text": {"content": "hello world"}}',
      signal: controller.signal
    })

    clearTimeout(timeoutId)

    if (response.ok) {
      alert($t('config.webhook_test_success'))
    } else {
      alert(`${$t('config.webhook_test_failed')}: ${response.status} ${response.statusText}`)
    }
  } catch (error) {
    if (error.name === 'AbortError') {
      const timeout = parseInt(config.value.webhook_timeout) || 1000
      alert($t('config.webhook_test_failed') + `: Request timeout (${timeout}ms)`)
    } else {
      alert(`${$t('config.webhook_test_failed')}: ${error.message}`)
    }
  }
}
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

    <!-- Port family -->
    <div class="mb-3">
      <label for="port" class="form-label">{{ $t('config.port') }}</label>
      <input type="number" min="1029" max="65514" class="form-control" id="port" :placeholder="defaultMoonlightPort"
             v-model="config.port" />
      <div class="form-text">{{ $t('config.port_desc') }}</div>
      <!-- Add warning if any port is less than 1024 -->
      <div class="alert alert-danger" v-if="(+effectivePort - 5) < 1024">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_1') }}
      </div>
      <!-- Add warning if any port is above 65535 -->
      <div class="alert alert-danger" v-if="(+effectivePort + 21) > 65535">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_2') }}
      </div>
      <!-- Create a port table for the various ports needed by Sunshine -->
      <table class="table">
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
      <!-- add warning about exposing web ui to the internet -->
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

    <!-- Webhook Settings -->
    <div class="accordion">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#webhook-collapse">
            {{ $t('config.webhook_group') }}
          </button>
        </h2>
        <div id="webhook-collapse" class="accordion-collapse collapse show">
          <div class="accordion-body">
            <!-- Webhook Enable -->
            <div class="mb-3">
              <label for="webhook_enabled" class="form-label">{{ $t('config.webhook_enabled') }}</label>
              <select id="webhook_enabled" class="form-select" v-model="config.webhook_enabled">
                <option value="disabled">{{ $t('_common.disabled_def') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.webhook_enabled_desc') }}</div>
            </div>

            <!-- Webhook URL -->
            <div class="mb-3" v-if="config.webhook_enabled === 'enabled'">
              <label for="webhook_url" class="form-label">{{ $t('config.webhook_url') }}</label>
              <div class="input-group">
                <input type="url" class="form-control" id="webhook_url" placeholder="https://example.com/webhook" v-model="config.webhook_url" />
                <button class="btn btn-outline-secondary" type="button" @click="testWebhook" :disabled="!config.webhook_url || config.webhook_enabled !== 'enabled'">
                  {{ $t('config.webhook_test') }}
                </button>
              </div>
              <div class="form-text">{{ $t('config.webhook_url_desc') }}</div>
            </div>

            <!-- Skip SSL Verify -->
            <div class="mb-3" v-if="config.webhook_enabled === 'enabled'">
              <label for="webhook_skip_ssl_verify" class="form-label">{{ $t('config.webhook_skip_ssl_verify') }}</label>
              <select id="webhook_skip_ssl_verify" class="form-select" v-model="config.webhook_skip_ssl_verify">
                <option value="disabled">{{ $t('_common.disabled_def') }}</option>
                <option value="enabled">{{ $t('_common.enabled') }}</option>
              </select>
              <div class="form-text">{{ $t('config.webhook_skip_ssl_verify_desc') }}</div>
            </div>

            <!-- Webhook Timeout -->
            <div class="mb-3" v-if="config.webhook_enabled === 'enabled'">
              <label for="webhook_timeout" class="form-label">{{ $t('config.webhook_timeout') }}</label>
              <input type="number" min="100" max="5000" class="form-control" id="webhook_timeout" placeholder="1000" v-model="config.webhook_timeout" />
              <div class="form-text">{{ $t('config.webhook_timeout_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>

  </div>
</template>

<style scoped>

</style>
