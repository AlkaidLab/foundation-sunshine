<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
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

</style>
