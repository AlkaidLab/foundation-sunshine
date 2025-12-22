<script setup>
import { ref, computed } from 'vue'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'

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

// 调用用户模式重启
const restartInUserMode = async () => {
  if (!isTauri.value) {
    showMessage('此功能仅在 Sunshine Control Panel 中可用', 'error')
    return
  }

  try {
    // 使用 inject-script.js 注入的 API 调用重启函数
    // 这会通过 postMessage 与父窗口通信，然后调用 Tauri API
    await window.__TAURI__.core.invoke('restart_sunshine_in_user_mode')
    showMessage('已请求以用户模式重启 Sunshine。如果弹出 UAC 提示，请点击"是"以确认。', 'success')
  } catch (error) {
    console.error('重启失败:', error)
    showMessage('重启失败: ' + (error.message || error), 'error')
  }
}
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
          class="btn btn-warning"
          style="white-space: nowrap;"
          @click="restartInUserMode"
          title="WGC 捕获需要在用户模式下运行。点击此按钮将以用户模式重启 Sunshine。"
        >
          <i class="fas fa-sync-alt me-1"></i>
          切换到用户模式
        </button>
      </div>
      <div class="form-text">
        {{ $t('config.capture_desc') }}
        <span v-if="isWGCSelected && isTauri" class="text-warning d-block mt-1">
          <i class="fas fa-exclamation-triangle me-1"></i>
          WGC 捕获需要在用户模式下运行。如果当前以服务模式运行，请点击上方按钮切换到用户模式。
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
