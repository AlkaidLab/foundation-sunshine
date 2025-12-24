<script setup>
import { ref, computed, onMounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'

const { t } = useI18n()

const props = defineProps(['platform', 'config', 'global_prep_cmd'])

const config = ref(props.config)

// 检查是否在 Tauri 环境中（通过 inject-script.js 注入）
const isTauri = computed(() => {
  return typeof window !== 'undefined' && window.__TAURI__?.core?.invoke
})

// 检查是否选择了 WGC
const isWGCSelected = computed(() => {
  return props.platform === 'windows' && config.value.capture === 'wgc'
})

// Sunshine 运行模式状态
const isUserMode = ref(false)
const isCheckingMode = ref(false)

const showMessage = (message, type = 'info') => {
  // 尝试使用 window.showToast（如果可用）
  if (typeof window.showToast === 'function') {
    window.showToast(message, type)
    return
  }

  // 尝试通过 postMessage 请求父窗口显示消息
  if (window.parent && window.parent !== window) {
    try {
      window.parent.postMessage(
        {
          type: 'show-message',
          message,
          messageType: type,
          source: 'sunshine-webui',
        },
        '*'
      )
      return
    } catch (e) {
      console.warn('无法通过 postMessage 发送消息:', e)
    }
  }

  // 降级到 alert
  if (type === 'error') {
    alert(message)
  } else {
    console.info(message)
  }
}

// 检查当前 Sunshine 运行模式
const checkSunshineMode = async () => {
  if (!isTauri.value) {
    return
  }

  isCheckingMode.value = true
  try {
    const result = await window.__TAURI__.core.invoke('is_sunshine_running_in_user_mode')
    isUserMode.value = result === true
  } catch (error) {
    console.error('检查 Sunshine 模式失败:', error)
    // 如果检查失败，默认假设为服务模式
    isUserMode.value = false
  } finally {
    isCheckingMode.value = false
  }
}

// 切换 Sunshine 运行模式
const toggleSunshineMode = async () => {
  if (!isTauri.value) {
    showMessage(t('config.wgc_control_panel_only'), 'error')
    return
  }

  try {
    const msg = await window.__TAURI__.core.invoke('toggle_sunshine_mode')
    showMessage(msg || t('config.wgc_mode_switch_started'), 'success')

    // 等待一段时间后重新检查模式
    setTimeout(() => {
      checkSunshineMode()
    }, 3000)
  } catch (error) {
    console.error('切换模式失败:', error)
    showMessage(t('config.wgc_mode_switch_failed') + ': ' + (error.message || error), 'error')
  }
}

onMounted(() => {
  if (isTauri.value && isWGCSelected.value) {
    checkSunshineMode()
  }
})

watch(isWGCSelected, (newValue) => {
  if (newValue && isTauri.value) {
    checkSunshineMode()
  }
})
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-3">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input type="text" class="form-control" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Quantization Parameter -->
    <div class="mb-3">
      <label for="qp" class="form-label">{{ $t('config.qp') }}</label>
      <input type="number" class="form-control" id="qp" placeholder="28" v-model="config.qp" />
      <div class="form-text">{{ $t('config.qp_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-3">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input type="number" class="form-control" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- HEVC Support -->
    <div class="mb-3">
      <label for="hevc_mode" class="form-label">{{ $t('config.hevc_mode') }}</label>
      <select id="hevc_mode" class="form-select" v-model="config.hevc_mode">
        <option value="0">{{ $t('config.hevc_mode_0') }}</option>
        <option value="1">{{ $t('config.hevc_mode_1') }}</option>
        <option value="2">{{ $t('config.hevc_mode_2') }}</option>
        <option value="3">{{ $t('config.hevc_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.hevc_mode_desc') }}</div>
    </div>

    <!-- AV1 Support -->
    <div class="mb-3">
      <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
      <select id="av1_mode" class="form-select" v-model="config.av1_mode">
        <option value="0">{{ $t('config.av1_mode_0') }}</option>
        <option value="1">{{ $t('config.av1_mode_1') }}</option>
        <option value="2">{{ $t('config.av1_mode_2') }}</option>
        <option value="3">{{ $t('config.av1_mode_3') }}</option>
      </select>
      <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
    </div>

    <!-- Capture -->
    <div class="mb-3" v-if="platform !== 'macos'">
      <label for="capture" class="form-label">{{ $t('config.capture') }}</label>
      <div class="d-flex align-items-center gap-2">
        <select id="capture" class="form-select flex-grow-1" v-model="config.capture">
          <option value="">{{ $t('_common.autodetect') }}</option>
          <PlatformLayout :platform="platform">
            <template #linux>
              <option value="nvfbc">NvFBC</option>
              <option value="wlr">wlroots</option>
              <option value="kms">KMS</option>
              <option value="x11">X11</option>
            </template>
            <template #windows>
              <option value="ddx">Desktop Duplication API</option>
              <option value="wgc">Windows.Graphics.Capture {{ $t('_common.beta') }}</option>
              <option value="amd">AMD Display Capture {{ $t('_common.beta') }}</option>
            </template>
          </PlatformLayout>
        </select>
        <button
          v-if="isWGCSelected && isTauri"
          type="button"
          :class="['btn', isUserMode ? 'btn-success' : 'btn-warning']"
          style="white-space: nowrap"
          @click="toggleSunshineMode"
          :disabled="isCheckingMode"
          :title="
            isUserMode
              ? $t('config.wgc_switch_to_service_mode_tooltip')
              : $t('config.wgc_switch_to_user_mode_tooltip')
          "
        >
          <i v-if="isCheckingMode" class="fas fa-spinner fa-spin me-1"></i>
          <i v-else class="fas fa-sync-alt me-1"></i>
          {{
            isCheckingMode
              ? $t('config.wgc_checking_mode')
              : isUserMode
                ? $t('config.wgc_switch_to_service_mode')
                : $t('config.wgc_switch_to_user_mode')
          }}
        </button>
      </div>
      <div class="form-text">
        {{ $t('config.capture_desc') }}
        <span v-if="isWGCSelected && isTauri" :class="['d-block mt-1', isUserMode ? 'text-success' : 'text-warning']">
          <i :class="['me-1', isUserMode ? 'fas fa-check-circle' : 'fas fa-exclamation-triangle']"></i>
          <span v-if="isCheckingMode">{{ $t('config.wgc_checking_running_mode') }}</span>
          <span v-else-if="isUserMode">{{ $t('config.wgc_user_mode_available') }}</span>
          <span v-else>{{ $t('config.wgc_service_mode_warning') }}</span>
        </span>
      </div>
    </div>

    <!-- Encoder -->
    <div class="mb-3">
      <label for="encoder" class="form-label">{{ $t('config.encoder') }}</label>
      <select id="encoder" class="form-select" v-model="config.encoder">
        <option value="">{{ $t('_common.autodetect') }}</option>
        <PlatformLayout :platform="platform">
          <template #windows>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="quicksync">Intel QuickSync</option>
            <option value="amdvce">AMD AMF/VCE</option>
          </template>
          <template #linux>
            <option value="nvenc">NVIDIA NVENC</option>
            <option value="vaapi">VA-API</option>
          </template>
          <template #macos>
            <option value="videotoolbox">VideoToolbox</option>
          </template>
        </PlatformLayout>
        <option value="software">{{ $t('config.encoder_software') }}</option>
      </select>
      <div class="form-text">{{ $t('config.encoder_desc') }}</div>
    </div>
  </div>
</template>

<style scoped></style>
