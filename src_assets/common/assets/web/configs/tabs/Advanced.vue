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

// 检查是否选择了 AMD Display Capture
const isAMDCaptureSelected = computed(() => {
  return props.platform === 'windows' && config.value.capture === 'amd'
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

    // 切换通过 UAC 提升的 PowerShell 在后台执行，需预留：UAC 确认 + net stop + taskkill + 启动。延迟后再检查，并做二次检查以修正中间状态。
    setTimeout(() => checkSunshineMode(), 6000)
    setTimeout(() => checkSunshineMode(), 11000)
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

// === 编解码器策略（HEVC + AV1 整合 UI） ===
// 底层仍写入 hevc_mode / av1_mode（保持 sunshine.conf 兼容），UI 层用一个策略 +
// 一个 HDR 复选框推算两者的值。
const showCodecAdvanced = ref(false)

// 把 config.value.hevc_mode / av1_mode 转成 number，便于比较（旧值可能是 string）
const hevcModeNum = computed(() => Number(config.value.hevc_mode ?? 0))
const av1ModeNum = computed(() => Number(config.value.av1_mode ?? 0))

const codecStrategy = computed({
  get() {
    const h = hevcModeNum.value
    const a = av1ModeNum.value
    if (h === 0 && a === 0) return 'auto'
    if (h === 1 && a === 1) return 'h264_only'
    // modern: 都通告（值为 2 或 3 都算 modern；HDR 由 enableHdr 单独决定）
    if ((h === 2 || h === 3) && (a === 2 || a === 3)) return 'modern'
    return 'custom'
  },
  set(v) {
    if (v === 'auto') {
      config.value.hevc_mode = 0
      config.value.av1_mode = 0
    } else if (v === 'h264_only') {
      config.value.hevc_mode = 1
      config.value.av1_mode = 1
    } else if (v === 'modern') {
      const hdr = enableHdr.value
      config.value.hevc_mode = hdr ? 3 : 2
      config.value.av1_mode = hdr ? 3 : 2
    }
    // 'custom' → 不修改值，由用户在展开区编辑
  },
})

// 当任意 codec 选择 mode 3（含 10-bit/HDR），即视为开启 HDR 通告
const enableHdr = computed({
  get() {
    return hevcModeNum.value === 3 || av1ModeNum.value === 3
  },
  set(v) {
    // 仅在"现代编码器"策略下生效
    if (codecStrategy.value !== 'modern') return
    config.value.hevc_mode = v ? 3 : 2
    config.value.av1_mode = v ? 3 : 2
  },
})

// HDR 复选框是否可用（只有 modern 策略下才有意义）
const hdrToggleDisabled = computed(() => codecStrategy.value !== 'modern')
</script>

<template>
  <div class="config-page">
    <!-- FEC Percentage -->
    <div class="mb-3">
      <label for="fec_percentage" class="form-label">{{ $t('config.fec_percentage') }}</label>
      <input type="text" class="form-control" id="fec_percentage" placeholder="20" v-model="config.fec_percentage" />
      <div class="form-text">{{ $t('config.fec_percentage_desc') }}</div>
    </div>

    <!-- Min Threads -->
    <div class="mb-3">
      <label for="min_threads" class="form-label">{{ $t('config.min_threads') }}</label>
      <input type="number" class="form-control" id="min_threads" placeholder="2" min="1" v-model="config.min_threads" />
      <div class="form-text">{{ $t('config.min_threads_desc') }}</div>
    </div>

    <!-- Codec Strategy (整合 HEVC + AV1) -->
    <div class="mb-3">
      <label for="codec_strategy" class="form-label">{{ $t('config.codec_strategy') }}</label>
      <select id="codec_strategy" class="form-select" v-model="codecStrategy">
        <option value="auto">{{ $t('config.codec_strategy_auto') }}</option>
        <option value="modern">{{ $t('config.codec_strategy_modern') }}</option>
        <option value="h264_only">{{ $t('config.codec_strategy_h264') }}</option>
        <option value="custom" disabled v-if="codecStrategy !== 'custom'">
          {{ $t('config.codec_strategy_custom_locked') }}
        </option>
        <option value="custom" v-else>{{ $t('config.codec_strategy_custom') }}</option>
      </select>

      <div class="form-check mt-2">
        <input
          class="form-check-input"
          type="checkbox"
          id="codec_enable_hdr"
          v-model="enableHdr"
          :disabled="hdrToggleDisabled"
        />
        <label class="form-check-label" for="codec_enable_hdr">
          {{ $t('config.codec_enable_hdr') }}
        </label>
        <div class="form-text" v-if="hdrToggleDisabled">
          {{ $t('config.codec_enable_hdr_disabled_hint') }}
        </div>
      </div>

      <div class="form-text">{{ $t('config.codec_strategy_desc') }}</div>

      <!-- 偏离推荐值时给出温和提示 -->
      <div class="alert alert-warning py-2 mt-2 mb-0" v-if="codecStrategy !== 'auto'">
        <small>{{ $t('config.codec_strategy_non_default_warning') }}</small>
      </div>

      <!-- 高级（专家模式）：原 HEVC / AV1 dropdown -->
      <div class="mt-2">
        <button
          type="button"
          class="btn btn-sm btn-outline-secondary"
          :aria-expanded="showCodecAdvanced"
          aria-controls="codec-advanced-panel"
          @click="showCodecAdvanced = !showCodecAdvanced"
        >
          {{ showCodecAdvanced ? $t('config.codec_advanced_hide') : $t('config.codec_advanced_show') }}
        </button>
      </div>

      <div v-if="showCodecAdvanced" id="codec-advanced-panel" class="mt-3 ps-3 border-start">
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

        <div class="mb-0">
          <label for="av1_mode" class="form-label">{{ $t('config.av1_mode') }}</label>
          <select id="av1_mode" class="form-select" v-model="config.av1_mode">
            <option value="0">{{ $t('config.av1_mode_0') }}</option>
            <option value="1">{{ $t('config.av1_mode_1') }}</option>
            <option value="2">{{ $t('config.av1_mode_2') }}</option>
            <option value="3">{{ $t('config.av1_mode_3') }}</option>
          </select>
          <div class="form-text">{{ $t('config.av1_mode_desc') }}</div>
        </div>
      </div>
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
              <option value="wgc">Windows Graphics Capture</option>
              <option value="amd">AMD Display Capture {{ $t('_common.beta') }}</option>
              <option value="vdd">ZakoVDD Direct Shared Texture {{ $t('_common.beta') }}</option>
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
        <span v-if="isAMDCaptureSelected" class="d-block mt-1 text-warning">
          <i class="fas fa-exclamation-triangle me-1"></i>
          {{ $t('config.amd_capture_no_virtual_display') }}
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
