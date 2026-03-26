<script setup>
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'

const { t } = useI18n()

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)

// Tauri 环境下的 vmouse 驱动管理
const isTauri = ref(false)
const vmouseStatus = ref({ installed: false, running: false, status_text: '' })
const vmouseLoading = ref(false)
const vmouseOperating = ref(false)

onMounted(async () => {
  if (window.isTauri && window.vmouseDriver) {
    isTauri.value = true
    await refreshVmouseStatus()
  }
})

async function refreshVmouseStatus() {
  vmouseLoading.value = true
  try {
    vmouseStatus.value = await window.vmouseDriver.getStatus()
  } catch { /* ignore */ }
  vmouseLoading.value = false
}

async function installVmouse() {
  if (!confirm(t('config.vmouse_confirm_install'))) return
  vmouseOperating.value = true
  try {
    await window.vmouseDriver.install()
    setTimeout(() => refreshVmouseStatus(), 2000)
  } catch (e) {
    alert(String(e))
  }
  vmouseOperating.value = false
}

async function uninstallVmouse() {
  if (!confirm(t('config.vmouse_confirm_uninstall'))) return
  vmouseOperating.value = true
  try {
    await window.vmouseDriver.uninstall()
    setTimeout(() => refreshVmouseStatus(), 2000)
  } catch (e) {
    alert(String(e))
  }
  vmouseOperating.value = false
}

const vmouseDotClass = computed(() => {
  if (vmouseStatus.value.running) return 'dot-active'
  if (vmouseStatus.value.installed) return 'dot-warning'
  return 'dot-inactive'
})

const vmouseStatusLabel = computed(() => {
  if (vmouseStatus.value.running) return t('config.vmouse_status_running')
  if (vmouseStatus.value.installed) return t('config.vmouse_status_installed')
  return t('config.vmouse_status_not_installed')
})
</script>

<template>
  <div id="input" class="config-page">
    <!-- Enable Gamepad Input -->
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="controller" 
               v-model="config.controller" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="controller">
          {{ $t('config.controller') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.controller_desc') }}</div>
    </div>

    <!-- Emulated Gamepad Type -->
    <div class="mb-3" v-if="config.controller === 'enabled' && platform !== 'macos'">
      <label for="gamepad" class="form-label">{{ $t('config.gamepad') }}</label>
      <select id="gamepad" class="form-select" v-model="config.gamepad">
        <option value="auto">{{ $t('_common.auto') }}</option>

        <PlatformLayout :platform="platform">
          <template #linux>
            <option value="ds5">{{ $t("config.gamepad_ds5") }}</option>
            <option value="switch">{{ $t("config.gamepad_switch") }}</option>
            <option value="xone">{{ $t("config.gamepad_xone") }}</option>
          </template>
          
          <template #windows>
            <option value="ds4">{{ $t('config.gamepad_ds4') }}</option>
            <option value="x360">{{ $t('config.gamepad_x360') }}</option>
          </template>
        </PlatformLayout>
      </select>
      <div class="form-text">{{ $t('config.gamepad_desc') }}</div>
    </div>

    <div class="accordion" v-if="config.gamepad === 'ds4'">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.gamepad_ds4_manual') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <div>
              <label for="ds4_back_as_touchpad_click" class="form-label">{{ $t('config.ds4_back_as_touchpad_click') }}</label>
              <select id="ds4_back_as_touchpad_click" class="form-select"
                      v-model="config.ds4_back_as_touchpad_click">
                <option value="disabled">{{ $t('_common.disabled') }}</option>
                <option value="enabled">{{ $t('_common.enabled_def') }}</option>
              </select>
              <div class="form-text">{{ $t('config.ds4_back_as_touchpad_click_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>
    <div class="accordion" v-if="config.controller === 'enabled' && config.gamepad === 'auto' && platform === 'windows'">
      <div class="accordion-item">
        <h2 class="accordion-header">
          <button class="accordion-button" type="button" data-bs-toggle="collapse"
                  data-bs-target="#panelsStayOpen-collapseOne">
            {{ $t('config.gamepad_auto') }}
          </button>
        </h2>
        <div id="panelsStayOpen-collapseOne" class="accordion-collapse collapse show"
             aria-labelledby="panelsStayOpen-headingOne">
          <div class="accordion-body">
            <div class="mb-3">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="motion_as_ds4" 
                       v-model="config.motion_as_ds4" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="motion_as_ds4">
                  {{ $t('config.motion_as_ds4') }}
                </label>
              </div>
              <div class="form-text">{{ $t('config.motion_as_ds4_desc') }}</div>
            </div>
            <div class="mb-3">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="touchpad_as_ds4" 
                       v-model="config.touchpad_as_ds4" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="touchpad_as_ds4">
                  {{ $t('config.touchpad_as_ds4') }}
                </label>
              </div>
              <div class="form-text">{{ $t('config.touchpad_as_ds4_desc') }}</div>
            </div>
            <div class="mb-3">
              <div class="form-check form-switch">
                <input class="form-check-input" type="checkbox" id="enable_dsu_server" 
                       v-model="config.enable_dsu_server" true-value="enabled" false-value="disabled">
                <label class="form-check-label" for="enable_dsu_server">
                  {{ $t('config.enable_dsu_server') }}
                </label>
              </div>
              <div class="form-text">{{ $t('config.enable_dsu_server_desc') }}</div>
            </div>
            <div class="mb-3" v-if="config.enable_dsu_server === 'enabled'">
              <label for="dsu_server_port" class="form-label">{{ $t('config.dsu_server_port') }}</label>
              <input type="number" class="form-control" id="dsu_server_port" placeholder="26760"
                     v-model="config.dsu_server_port" min="1024" max="65535" />
              <div class="form-text">{{ $t('config.dsu_server_port_desc') }}</div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Home/Guide Button Emulation Timeout -->
    <div class="mb-3" v-if="config.controller === 'enabled'">
      <label for="back_button_timeout" class="form-label">{{ $t('config.back_button_timeout') }}</label>
      <input type="text" class="form-control" id="back_button_timeout" placeholder="-1"
             v-model="config.back_button_timeout" />
      <div class="form-text">{{ $t('config.back_button_timeout_desc') }}</div>
    </div>

    <!-- Enable Keyboard Input -->
    <hr>
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="keyboard" 
               v-model="config.keyboard" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="keyboard">
          {{ $t('config.keyboard') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.keyboard_desc') }}</div>
    </div>

    <!-- Key Repeat Delay-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_delay" class="form-label">{{ $t('config.key_repeat_delay') }}</label>
      <input type="text" class="form-control" id="key_repeat_delay" placeholder="500"
             v-model="config.key_repeat_delay" />
      <div class="form-text">{{ $t('config.key_repeat_delay_desc') }}</div>
    </div>

    <!-- Key Repeat Frequency-->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <label for="key_repeat_frequency" class="form-label">{{ $t('config.key_repeat_frequency') }}</label>
      <input type="text" class="form-control" id="key_repeat_frequency" placeholder="24.9"
             v-model="config.key_repeat_frequency" />
      <div class="form-text">{{ $t('config.key_repeat_frequency_desc') }}</div>
    </div>

    <!-- Always send scancodes -->
    <div class="mb-3" v-if="config.keyboard === 'enabled' && platform === 'windows'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="always_send_scancodes" 
               v-model="config.always_send_scancodes" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="always_send_scancodes">
          {{ $t('config.always_send_scancodes') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.always_send_scancodes_desc') }}</div>
    </div>

    <!-- Mapping Key AltRight to Key Windows -->
    <div class="mb-3" v-if="config.keyboard === 'enabled'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="key_rightalt_to_key_win" 
               v-model="config.key_rightalt_to_key_win" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="key_rightalt_to_key_win">
          {{ $t('config.key_rightalt_to_key_win') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.key_rightalt_to_key_win_desc') }}</div>
    </div>

    <!-- Enable Mouse Input -->
    <hr>
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="mouse" 
               v-model="config.mouse" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="mouse">
          {{ $t('config.mouse') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.mouse_desc') }}</div>
    </div>

    <!-- High resolution scrolling support -->
    <div class="mb-3" v-if="config.mouse === 'enabled'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="high_resolution_scrolling" 
               v-model="config.high_resolution_scrolling" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="high_resolution_scrolling">
          {{ $t('config.high_resolution_scrolling') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.high_resolution_scrolling_desc') }}</div>
    </div>

    <!-- Native pen/touch support -->
    <div class="mb-3" v-if="config.mouse === 'enabled'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="native_pen_touch" 
               v-model="config.native_pen_touch" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="native_pen_touch">
          {{ $t('config.native_pen_touch') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.native_pen_touch_desc') }}</div>
    </div>

    <!-- Virtual mouse driver -->
    <div class="mb-3" v-if="config.mouse === 'enabled' && platform === 'windows'">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="virtual_mouse"
               v-model="config.virtual_mouse" true-value="enabled" false-value="disabled">
        <label class="form-check-label" for="virtual_mouse">
          {{ $t('config.virtual_mouse') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.virtual_mouse_desc') }}</div>

      <!-- Tauri 环境：驱动管理面板 -->
      <div v-if="isTauri" class="vmouse-panel mt-2">
        <div class="vmouse-panel-header">
          <div class="vmouse-status-indicator">
            <span class="vmouse-dot" :class="vmouseDotClass"></span>
            <span class="vmouse-status-label">{{ vmouseStatusLabel }}</span>
          </div>
          <button class="vmouse-refresh-btn" @click="refreshVmouseStatus"
                  :disabled="vmouseLoading" :title="$t('config.vmouse_refresh')">
            <i class="fas fa-sync-alt" :class="{ 'fa-spin': vmouseLoading }"></i>
          </button>
        </div>
        <div class="vmouse-panel-body">
          <button v-if="!vmouseStatus.installed" class="vmouse-action-btn vmouse-install-btn"
                  @click="installVmouse" :disabled="vmouseOperating">
            <span v-if="vmouseOperating" class="vmouse-spinner"></span>
            <i v-else class="fas fa-download"></i>
            <span>{{ vmouseOperating ? $t('config.vmouse_installing') : $t('config.vmouse_install') }}</span>
          </button>
          <button v-else class="vmouse-action-btn vmouse-uninstall-btn"
                  @click="uninstallVmouse" :disabled="vmouseOperating">
            <span v-if="vmouseOperating" class="vmouse-spinner"></span>
            <i v-else class="fas fa-trash-alt"></i>
            <span>{{ vmouseOperating ? $t('config.vmouse_uninstalling') : $t('config.vmouse_uninstall') }}</span>
          </button>
        </div>
      </div>

      <!-- 非 Tauri 环境：显示提示信息 -->
      <div v-else class="vmouse-helper mt-2">
        <i class="fas fa-info-circle me-1 text-info"></i>
        <span>{{ $t('config.vmouse_note') }}</span>
      </div>
    </div>

    <!-- Draw mouse cursor in AMF -->
    <div class="mb-3">
      <div class="form-check form-switch">
        <input class="form-check-input" type="checkbox" id="amf_draw_mouse_cursor" 
               v-model="config.amf_draw_mouse_cursor">
        <label class="form-check-label" for="amf_draw_mouse_cursor">
          {{ $t('config.amf_draw_mouse_cursor') }}
        </label>
      </div>
      <div class="form-text">{{ $t('config.amf_draw_mouse_cursor_desc') }}</div>
    </div>

  </div>
</template>

<style scoped>
/* 非 Tauri 环境的提示信息 */
.vmouse-helper {
  display: flex;
  align-items: center;
  padding: 0.5rem 0.75rem;
  background: var(--bs-secondary-bg);
  border-radius: 8px;
  border: 1px solid var(--bs-border-color);
  color: var(--bs-secondary-color);
  font-size: 0.85rem;
}

[data-bs-theme='dark'] .vmouse-helper {
  background: rgba(255, 255, 255, 0.05);
  border-color: rgba(255, 255, 255, 0.1);
}

/* 驱动管理面板 */
.vmouse-panel {
  border-radius: 10px;
  border: 1px solid var(--bs-border-color);
  overflow: hidden;
  transition: border-color 0.2s ease;
}

.vmouse-panel:hover {
  border-color: var(--bs-primary);
}

[data-bs-theme='dark'] .vmouse-panel {
  border-color: rgba(255, 255, 255, 0.1);
}

[data-bs-theme='dark'] .vmouse-panel:hover {
  border-color: rgba(var(--bs-primary-rgb), 0.5);
}

/* 面板头部 */
.vmouse-panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.6rem 0.85rem;
  background: var(--bs-tertiary-bg);
}

[data-bs-theme='dark'] .vmouse-panel-header {
  background: rgba(255, 255, 255, 0.04);
}

/* 状态指示器 */
.vmouse-status-indicator {
  display: flex;
  align-items: center;
  gap: 0.5rem;
}

.vmouse-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.vmouse-dot.dot-active {
  background: #22c55e;
  box-shadow: 0 0 6px rgba(34, 197, 94, 0.5);
  animation: vmouse-pulse 2s ease-in-out infinite;
}

.vmouse-dot.dot-warning {
  background: #f59e0b;
  box-shadow: 0 0 6px rgba(245, 158, 11, 0.4);
}

.vmouse-dot.dot-inactive {
  background: #9ca3af;
}

@keyframes vmouse-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.vmouse-status-label {
  font-size: 0.8rem;
  font-weight: 500;
  color: var(--bs-body-color);
  opacity: 0.85;
}

/* 刷新按钮 */
.vmouse-refresh-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  border: none;
  border-radius: 6px;
  background: transparent;
  color: var(--bs-secondary-color);
  cursor: pointer;
  transition: all 0.15s ease;
}

.vmouse-refresh-btn:hover:not(:disabled) {
  background: var(--bs-secondary-bg);
  color: var(--bs-body-color);
}

.vmouse-refresh-btn:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

/* 面板操作区 */
.vmouse-panel-body {
  padding: 0.6rem 0.85rem;
}

/* 操作按钮 */
.vmouse-action-btn {
  display: inline-flex;
  align-items: center;
  gap: 0.4rem;
  padding: 0.35rem 0.85rem;
  border: 1px solid transparent;
  border-radius: 6px;
  font-size: 0.8rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.15s ease;
}

.vmouse-action-btn:disabled {
  opacity: 0.65;
  cursor: not-allowed;
}

.vmouse-install-btn {
  background: var(--bs-primary);
  color: #fff;
  border-color: var(--bs-primary);
}

.vmouse-install-btn:hover:not(:disabled) {
  filter: brightness(1.1);
}

.vmouse-uninstall-btn {
  background: transparent;
  color: var(--bs-danger);
  border-color: var(--bs-danger);
}

.vmouse-uninstall-btn:hover:not(:disabled) {
  background: var(--bs-danger);
  color: #fff;
}

/* 操作中 spinner */
.vmouse-spinner {
  width: 14px;
  height: 14px;
  border: 2px solid currentColor;
  border-top-color: transparent;
  border-radius: 50%;
  animation: vmouse-spin 0.6s linear infinite;
}

@keyframes vmouse-spin {
  to { transform: rotate(360deg); }
}
</style>
