<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'

const props = defineProps([
  'platform',
  'config'
])

const { t } = useI18n()

const defaultMoonlightPort = 47989

const config = ref(props.config)
const effectivePort = computed(() => +config.value?.port ?? defaultMoonlightPort)
const showCurlModal = ref(false)
const copied = ref(false)

const curlCommand = computed(() => {
  if (!config.value.webhook_url) {
    return ''
  }
  
  const url = config.value.webhook_url
  const payload = JSON.stringify({
    msgtype: 'text',
    text: {
      content: 'Hello, Sunshine Foundation Webhook'
    }
  })
  
  // 转义 JSON 中的双引号，以便在双引号字符串中使用
  const escapedPayload = payload.replace(/"/g, '\\"')
  
  return `curl -X POST "${url}" -H "Content-Type: application/json" -d "${escapedPayload}"`
})

const showCurlCommand = () => {
  showCurlModal.value = true
  copied.value = false
}

const closeCurlModal = () => {
  showCurlModal.value = false
  copied.value = false
}

const copyCurlCommand = async () => {
  try {
    await navigator.clipboard.writeText(curlCommand.value)
    copied.value = true
    setTimeout(() => {
      copied.value = false
    }, 2000)
  } catch (error) {
    // 降级方案：使用传统方法
    const textArea = document.createElement('textarea')
    textArea.value = curlCommand.value
    textArea.style.position = 'fixed'
    textArea.style.opacity = '0'
    document.body.appendChild(textArea)
    textArea.select()
    try {
      document.execCommand('copy')
      copied.value = true
      setTimeout(() => {
        copied.value = false
      }, 2000)
    } catch (err) {
      alert(t('config.webhook_curl_copy_failed') || '复制失败，请手动选择并复制')
    }
    document.body.removeChild(textArea)
  }
}

const testWebhook = async () => {
  if (!config.value.webhook_url) {
    alert(t('config.webhook_test_url_required'))
    return
  }

  try {
    new URL(config.value.webhook_url)
  } catch (error) {
    alert(t('config.webhook_test_failed') + ': Invalid URL format')
    return
  }

  try {
    const testPayload = JSON.stringify({
      msgtype: 'text',
      text: {
        content: 'Sunshine Webhook Test - This is a test message from Sunshine configuration page'
      }
    })

    const controller = new AbortController()
    const timeout = parseInt(config.value.webhook_timeout) || 1000
    const timeoutId = setTimeout(() => controller.abort(), timeout)

    try {
      const response = await fetch(config.value.webhook_url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: testPayload,
        signal: controller.signal
      })

      clearTimeout(timeoutId)

      if (response.ok) {
        alert(t('config.webhook_test_success'))
        return
      } else {
        throw new Error(`HTTP ${response.status}`)
      }
    } catch (corsError) {
      clearTimeout(timeoutId)

      if (corsError.name === 'TypeError' && corsError.message.includes('Failed to fetch')) {
        const noCorsController = new AbortController()
        const noCorsTimeoutId = setTimeout(() => noCorsController.abort(), timeout)

        try {
          await fetch(config.value.webhook_url, {
            method: 'POST',
            mode: 'no-cors',
            headers: {
              'Content-Type': 'application/json',
            },
            body: testPayload,
            signal: noCorsController.signal
          })

          clearTimeout(noCorsTimeoutId)
          alert(t('config.webhook_test_success') + '\n\n' + t('config.webhook_test_success_cors_note'))
        } catch (noCorsError) {
          clearTimeout(noCorsTimeoutId)
          if (noCorsError.name === 'AbortError') {
            throw new Error(`Request timeout (${timeout}ms)`)
          }
          throw noCorsError
        }
      } else if (corsError.name === 'AbortError') {
        throw new Error(`Request timeout (${timeout}ms)`)
      } else {
        throw corsError
      }
    }
  } catch (error) {
    if (error.name === 'AbortError' || error.message.includes('timeout')) {
      const timeout = parseInt(config.value.webhook_timeout) || 1000
      alert(t('config.webhook_test_failed') + `: Request timeout (${timeout}ms)`)
    } else {
      alert(`${t('config.webhook_test_failed')}: ${error.message || 'Unknown error'}\n\n${t('config.webhook_test_failed_note')}`)
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
                <button class="btn btn-outline-info" type="button" @click="testWebhook" :disabled="!config.webhook_url || config.webhook_enabled !== 'enabled'">
                  <i class="fas fa-paper-plane me-1"></i>{{ $t('config.webhook_test') }}
                </button>
                <button class="btn btn-outline-info" type="button" @click="showCurlCommand" :disabled="!config.webhook_url || config.webhook_enabled !== 'enabled'">
                  <i class="fas fa-terminal me-1"></i>{{ $t('config.webhook_curl_command') }}
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

  <!-- Curl Command Modal -->
  <Transition name="fade">
    <div v-if="showCurlModal" class="curl-command-overlay" @click.self="closeCurlModal">
      <div class="curl-command-modal">
        <div class="curl-command-header">
          <h5>
            <i class="fas fa-terminal me-2"></i>{{ $t('config.webhook_curl_command') || 'Curl 命令' }}
          </h5>
          <button class="btn-close" @click="closeCurlModal"></button>
        </div>
        <div class="curl-command-body">
          <p class="text-muted mb-3">{{ $t('config.webhook_curl_command_desc') || '复制以下命令到终端中执行，可以测试 webhook 是否正常工作：' }}</p>
          <div class="curl-command-container">
            <pre class="curl-command" id="curlCommandText">{{ curlCommand }}</pre>
          </div>
          <div class="alert alert-info mt-3" v-if="copied">
            <i class="fas fa-check-circle me-2"></i>{{ $t('_common.copied') || '已复制到剪贴板' }}
          </div>
        </div>
        <div class="curl-command-footer">
          <button class="copy-btn" @click="copyCurlCommand" type="button">
            <i class="fas fa-copy me-1"></i>{{ $t('_common.copy') }}
          </button>
          <button type="button" class="btn btn-secondary" @click="closeCurlModal">{{ $t('_common.close') || '关闭' }}</button>
        </div>
      </div>
    </div>
  </Transition>
</template>

<style scoped>
/* Curl Command Modal - 使用 ScanResultModal 样式 */
.curl-command-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  width: 100vw;
  height: 100vh;
  margin: 0;
  background: var(--overlay-bg, rgba(0, 0, 0, 0.7));
  backdrop-filter: blur(8px);
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 20px);
  overflow: hidden;
  
  [data-bs-theme='light'] & {
    background: rgba(0, 0, 0, 0.5);
  }
}

.curl-command-modal {
  background: var(--modal-bg, rgba(30, 30, 50, 0.95));
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.2));
  border-radius: var(--border-radius-xl, 12px);
  width: 100%;
  max-width: 700px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  backdrop-filter: blur(20px);
  box-shadow: var(--shadow-xl, 0 25px 50px rgba(0, 0, 0, 0.5));
  animation: modalSlideUp 0.3s ease;
  
  [data-bs-theme='light'] & {
    background: rgba(255, 255, 255, 0.95);
    border: 1px solid rgba(0, 0, 0, 0.15);
    box-shadow: 0 25px 50px rgba(0, 0, 0, 0.2);
  }
}

@keyframes modalSlideUp {
  from {
    transform: translateY(20px);
    opacity: 0;
  }
  to {
    transform: translateY(0);
    opacity: 1;
  }
}

.curl-command-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-bottom: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  h5 {
    margin: 0;
    color: var(--text-primary, #fff);
    font-size: var(--font-size-lg, 1.1rem);
    font-weight: 600;
    display: flex;
    align-items: center;
    gap: var(--spacing-sm, 8px);
  }
  
  [data-bs-theme='light'] & {
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);
    
    h5 {
      color: #000000;
    }
  }
}

.curl-command-body {
  padding: var(--spacing-lg, 24px);
  overflow-y: auto;
  flex: 1;
  color: var(--text-primary, #fff);
  
  p, span, div {
    color: var(--text-primary, #fff);
  }
  
  .text-muted {
    color: rgba(255, 255, 255, 0.6);
  }
  
  .alert {
    color: var(--text-primary, #fff);
    
    &-info {
      background: rgba(23, 162, 184, 0.2);
      border-color: rgba(23, 162, 184, 0.5);
    }
  }
  
  [data-bs-theme='light'] & {
    color: #000000;
    
    p, span, div {
      color: #000000;
    }
    
    .text-muted {
      color: rgba(0, 0, 0, 0.6);
    }
    
    .alert {
      color: #000000;
      
      &-info {
        background: rgba(23, 162, 184, 0.15);
        border-color: rgba(23, 162, 184, 0.4);
      }
    }
  }
}

.curl-command-footer {
  display: flex;
  justify-content: flex-end;
  align-items: center;
  gap: 10px;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-top: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));
  
  [data-bs-theme='light'] & {
    border-top: 1px solid rgba(0, 0, 0, 0.1);
  }
}

.curl-command-container {
  position: relative;
  background-color: rgba(255, 255, 255, 0.05);
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));
  border-radius: 4px;
  padding: 15px;
  
  [data-bs-theme='light'] & {
    background-color: rgba(248, 249, 250, 0.8);
    border: 1px solid rgba(0, 0, 0, 0.15);
  }
}

.curl-command {
  margin: 0;
  padding: 0;
  font-family: 'Courier New', monospace;
  font-size: 0.9rem;
  color: var(--text-primary, #fff);
  background: transparent;
  border: none;
  white-space: pre-wrap;
  word-break: break-all;
  overflow-x: auto;
  max-height: 300px;
  overflow-y: auto;
  
  [data-bs-theme='light'] & {
    color: #000000;
  }
}


/* Vue 过渡动画 */
.fade-enter-active {
  transition: opacity 0.3s ease;
}

.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}
</style>
