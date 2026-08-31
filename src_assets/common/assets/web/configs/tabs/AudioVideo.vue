<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../platform-i18n'
import { openExternalUrl } from '../../utils/helpers.js'
import { apiJson, apiPostJson } from '../../utils/apiFetch.js'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'
import NewDisplayOutputSelector from './audiovideo/NewDisplayOutputSelector.vue'
import DisplayDeviceOptions from './audiovideo/DisplayDeviceOptions.vue'
import DisplayModesSettings from './audiovideo/DisplayModesSettings.vue'
import VirtualDisplaySettings from './audiovideo/VirtualDisplaySettings.vue'
import AutomaticNumberSetting from './audiovideo/AutomaticNumberSetting.vue'
import Checkbox from '../../components/Checkbox.vue'
import ConfirmDialog from '../../components/common/ConfirmDialog.vue'

const props = defineProps(['platform', 'config', 'resolutions', 'fps', 'displayModeRemapping'])

const { t } = useI18n()
const config = ref(props.config)
const currentSubTab = ref('display-modes')
const showDownloadConfirm = ref(false)
const micTestRunning = ref(false)
const micTestResult = ref(null)
const micStatusLoading = ref(false)
const micStatus = ref(null)
let micStatusTimer

const microphoneBackend = computed(() => config.value.microphone_redirect_backend || 'vb_cable')
const showVbCableActions = computed(() => ['vb_cable', 'auto'].includes(microphoneBackend.value))
const showUsbipStatus = computed(() => ['usbip_experimental', 'auto'].includes(microphoneBackend.value))
const micStatusClass = computed(() => {
  if (!micStatus.value) return 'text-bg-secondary'
  if (micStatus.value.online && micStatus.value.device_created) return 'text-bg-success'
  if (micStatus.value.error_code) return 'text-bg-danger'
  return 'text-bg-secondary'
})

const refreshMicrophoneStatus = async ({ quiet = false } = {}) => {
  if (props.platform !== 'windows') return
  if (!quiet) micStatusLoading.value = true
  try {
    micStatus.value = await apiJson('/api/microphone/status')
  } catch {
    if (!quiet) micStatus.value = null
  } finally {
    if (!quiet) micStatusLoading.value = false
  }
}

const handleDownloadVSink = () => {
  showDownloadConfirm.value = true
}

const confirmDownload = async () => {
  showDownloadConfirm.value = false
  const url = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip'
  
  try {
    await openExternalUrl(url)
  } catch (error) {
    console.error('Failed to open URL:', error)
  }
}

const cancelDownload = () => {
  showDownloadConfirm.value = false
}

const testMicrophoneRoute = async () => {
  micTestRunning.value = true
  micTestResult.value = null

  try {
    const result = await apiPostJson('/api/microphone/test')
    micTestResult.value = {
      success: result.success === true,
      backend: result.backend || '',
      errorCode: result.error_code || '',
      messageKey: result.success === true
        ? (result.backend === 'usbip_experimental'
            ? 'config.stream_mic_test_success_usbip'
            : 'config.stream_mic_test_success')
        : (
            result.error_code === 'MIC_TEST_DEVICE_UNAVAILABLE'
              ? 'config.stream_mic_test_device_unavailable'
              : result.error_code === 'MIC_USBIP_COMPONENT_UNAVAILABLE'
                ? 'config.stream_mic_test_usbip_unavailable'
              : 'config.stream_mic_test_failed'
          ),
    }
  } catch {
    micTestResult.value = {
      success: false,
      messageKey: 'config.stream_mic_test_failed',
    }
  } finally {
    micTestRunning.value = false
    await refreshMicrophoneStatus({ quiet: true })
  }
}

watch(microphoneBackend, () => {
  micTestResult.value = null
})

onMounted(() => {
  refreshMicrophoneStatus()
  micStatusTimer = window.setInterval(() => refreshMicrophoneStatus({ quiet: true }), 3000)
})

onBeforeUnmount(() => {
  if (micStatusTimer) window.clearInterval(micStatusTimer)
})
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input
        type="text"
        class="form-control"
        id="audio_sink"
        :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
        v-model="config.audio_sink"
      />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br />
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br />
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input
            type="text"
            class="form-control"
            id="virtual_sink"
            :placeholder="$t('config.virtual_sink_placeholder')"
            v-model="config.virtual_sink"
          />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <div class="mb-3">
          <label for="install_steam_audio_drivers" class="form-label">{{
            $t('config.install_steam_audio_drivers')
          }}</label>
          <select id="install_steam_audio_drivers" class="form-select" v-model="config.install_steam_audio_drivers">
            <option value="disabled">{{ $t('_common.disabled') }}</option>
            <option value="enabled">{{ $t('_common.enabled_def') }}</option>
          </select>
          <div class="form-text">{{ $t('config.install_steam_audio_drivers_desc') }}</div>
        </div>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox
      class="mb-3"
      id="stream_audio"
      locale-prefix="config"
      v-model="config.stream_audio"
      default="true"
    ></Checkbox>

    <!-- Disable Microphone -->
    <div class="mb-3">
      <Checkbox
        id="stream_mic"
        locale-prefix="config"
        v-model="config.stream_mic"
        default="true"
      ></Checkbox>
      <PlatformLayout :platform="platform">
        <template #windows>
          <div class="stream-mic-backend mt-3">
            <label for="microphone_redirect_backend" class="form-label fw-semibold">
              {{ $t('config.microphone_redirect_backend') }}
            </label>
            <select
              id="microphone_redirect_backend"
              class="form-select"
              v-model="config.microphone_redirect_backend"
            >
              <option value="auto">{{ $t('config.microphone_redirect_backend_auto') }}</option>
              <option value="usbip_experimental">
                {{ $t('config.microphone_redirect_backend_usbip') }}
              </option>
              <option value="vb_cable">{{ $t('config.microphone_redirect_backend_vb') }}</option>
              <option value="disabled">{{ $t('config.microphone_redirect_backend_disabled') }}</option>
            </select>
            <div class="form-text">{{ $t('config.microphone_redirect_backend_desc') }}</div>
            <div v-if="showUsbipStatus" class="alert alert-warning mt-2 mb-0 py-2" role="note">
              <i class="fas fa-flask me-2"></i>{{ $t('config.microphone_redirect_usbip_warning') }}
            </div>
          </div>
        </template>
      </PlatformLayout>
      <div class="stream-mic-helper mt-2">
        <button
          v-if="platform !== 'windows' || showVbCableActions"
          type="button"
          class="btn btn-sm btn-primary stream-mic-download-btn"
          @click="handleDownloadVSink"
        >
          <i class="fas fa-download me-1"></i>
          {{ $t('_common.download') }}
        </button>
        <button
          v-if="platform === 'windows'"
          type="button"
          class="btn btn-sm btn-outline-primary stream-mic-test-btn"
          :disabled="micTestRunning || microphoneBackend === 'disabled'"
          @click="testMicrophoneRoute"
        >
          <i class="fas fa-wave-square me-1"></i>
          {{ micTestRunning ? $t('config.stream_mic_testing') : $t('config.stream_mic_test') }}
        </button>
        <div class="stream-mic-note">
          <i class="fas fa-info-circle me-2"></i>
          <span>
            {{
              platform === 'windows'
                ? $t(showUsbipStatus ? 'config.stream_mic_test_note_usbip' : 'config.stream_mic_test_note')
                : $t('config.stream_mic_note')
            }}
          </span>
        </div>
      </div>
      <div
        v-if="micTestResult"
        class="alert mt-2 mb-0 py-2"
        :class="micTestResult.success ? 'alert-success' : 'alert-danger'"
        role="status"
        aria-live="polite"
      >
        {{ $t(micTestResult.messageKey) }}
        <code v-if="micTestResult.errorCode" class="d-block mt-1">{{ micTestResult.errorCode }}</code>
      </div>

      <section v-if="platform === 'windows'" class="stream-mic-status mt-3">
        <div class="stream-mic-status__header">
          <div>
            <h6 class="mb-1">{{ $t('config.microphone_redirect_status') }}</h6>
            <span class="badge" :class="micStatusClass" aria-live="polite">
              {{ micStatus ? $t(`config.microphone_redirect_state_${micStatus.state || 'absent'}`) : $t('config.microphone_redirect_state_unknown') }}
            </span>
          </div>
          <button
            type="button"
            class="btn btn-sm btn-outline-secondary"
            :disabled="micStatusLoading"
            @click="refreshMicrophoneStatus()"
          >
            <i class="fas fa-rotate me-1" :class="{ 'fa-spin': micStatusLoading }"></i>
            {{ $t('config.microphone_redirect_refresh') }}
          </button>
        </div>

        <div v-if="micStatus" class="stream-mic-status__grid mt-3">
          <div><span>{{ $t('config.microphone_redirect_active_backend') }}</span><strong>{{ micStatus.active_backend || '—' }}</strong></div>
          <div><span>{{ $t('config.microphone_redirect_component') }}</span><strong>{{ micStatus.component_available ? $t('config.microphone_redirect_available') : $t('config.microphone_redirect_unavailable') }}</strong></div>
          <div><span>{{ $t('config.microphone_redirect_endpoint') }}</span><strong>{{ micStatus.device_created ? $t('config.microphone_redirect_available') : $t('config.microphone_redirect_unavailable') }}</strong></div>
          <div><span>{{ $t('config.microphone_redirect_host_capture') }}</span><strong>{{ micStatus.host_streaming ? $t('config.microphone_redirect_active') : $t('config.microphone_redirect_inactive') }}</strong></div>
          <div><span>{{ $t('config.microphone_redirect_buffered') }}</span><strong>{{ micStatus.buffered_bytes || 0 }} B</strong></div>
          <div><span>{{ $t('config.microphone_redirect_diagnostics') }}</span><strong>{{ micStatus.underruns || 0 }} / {{ micStatus.dropped_frames || 0 }} / {{ micStatus.submit_errors || 0 }}</strong></div>
        </div>
        <div v-if="micStatus?.fallback_reason" class="form-text mt-2">
          {{ $t('config.microphone_redirect_fallback') }}: <code>{{ micStatus.fallback_reason }}</code>
        </div>
        <div v-if="micStatus?.error_code" class="form-text text-danger mt-2">
          {{ $t('config.microphone_redirect_error') }}: <code>{{ micStatus.error_code }}</code>
        </div>
        <div class="form-text mt-2">{{ $t('config.microphone_redirect_apply_note') }}</div>
      </section>
    </div>

    <section class="stream-encoding-limit mb-3">
      <AutomaticNumberSetting
        v-model="config.max_bitrate"
        id="max_bitrate"
        label-key="config.max_bitrate"
        description-key="config.max_bitrate_desc"
        icon="fa-gauge-high"
        unit="Kbps"
      />
    </section>

    <NewDisplayOutputSelector :platform="platform" :config="config" />

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
      :display-mode-remapping="displayModeRemapping"
    />

    <!-- Display Modes Tab Navigation -->
    <div class="mb-3">
      <ul class="nav nav-tabs audio-video-tabs">
        <li class="nav-item">
          <a
            class="nav-link"
            :class="{ active: currentSubTab === 'display-modes' }"
            href="#"
            @click.prevent="currentSubTab = 'display-modes'"
          >
            {{ $t('config.display_modes') || 'Display Modes' }}
          </a>
        </li>
        <li class="nav-item">
          <a
            class="nav-link"
            :class="{ active: currentSubTab === 'virtual-display' }"
            href="#"
            @click.prevent="currentSubTab = 'virtual-display'"
          >
            {{ $t('config.virtual_display') || 'Virtual Display' }}
          </a>
        </li>
      </ul>

      <!-- Display Modes Tab Content -->
      <div class="tab-content">
        <DisplayModesSettings
          v-if="currentSubTab === 'display-modes'"
          :config="config"
        />
        
        <!-- Virtual Display Tab Content -->
        <VirtualDisplaySettings
          v-if="currentSubTab === 'virtual-display'"
          :platform="platform"
          :config="config"
          :resolutions="resolutions"
          :fps="fps"
        />
      </div>
    </div>

    <ConfirmDialog
      :show="showDownloadConfirm"
      dialog-id="stream-mic-download-confirm"
      :title="$t('_common.download')"
      title-icon="fas fa-external-link-alt"
      :close-label="$t('_common.close')"
      @close="cancelDownload"
    >
      <p>{{ $t('config.stream_mic_download_confirm') }}</p>
      <template #actions>
        <button type="button" class="btn btn-secondary" @click="cancelDownload">{{ $t('_common.cancel') }}</button>
        <button type="button" class="btn btn-primary" @click="confirmDownload">
          <i class="fas fa-download me-1"></i>{{ $t('_common.download') }}
        </button>
      </template>
    </ConfirmDialog>
  </div>
</template>

<style scoped>
.nav-tabs {
  gap: 0.25rem;
  padding: 0.3rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  margin-bottom: 0.75rem;
}

.nav-tabs .nav-link {
  border: none;
  border-radius: var(--ui-radius-sm);
  color: var(--ui-text-secondary);
  padding: 0.65rem 1rem;
  transition: color 0.2s ease, background-color 0.2s ease, box-shadow 0.2s ease;
}

.nav-tabs .nav-link:hover {
  background: var(--ui-surface-hover);
  color: var(--ui-text-primary);
}

.nav-tabs .nav-link.active {
  color: var(--ui-accent);
  background: var(--ui-surface-strong);
  box-shadow: var(--ui-shadow-sm);
  font-weight: 600;
}

.tab-content {
  padding-top: 0;
}

.stream-encoding-limit {
  padding: 1rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
}

.stream-mic-helper {
  display: flex;
  align-items: center;
  gap: 1rem;
  flex-wrap: wrap;
  padding: 0.75rem;
  background: var(--ui-surface);
  border-radius: var(--ui-radius-md);
  border: 1px solid var(--ui-border);
}

.stream-mic-backend,
.stream-mic-status {
  padding: 1rem;
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
}

.stream-mic-status__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
}

.stream-mic-status__grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(145px, 1fr));
  gap: 0.75rem;
}

.stream-mic-status__grid > div {
  display: flex;
  flex-direction: column;
  gap: 0.2rem;
  min-width: 0;
}

.stream-mic-status__grid span {
  color: var(--ui-text-secondary);
  font-size: 0.78rem;
}

.stream-mic-status__grid strong {
  overflow-wrap: anywhere;
  font-size: 0.9rem;
}

.stream-mic-download-btn {
  white-space: nowrap;
  flex-shrink: 0;
  order: -1;
}

.stream-mic-test-btn {
  white-space: nowrap;
  flex-shrink: 0;
}

.stream-mic-note {
  display: flex;
  align-items: center;
  color: var(--ui-text-secondary);
  font-size: 0.875rem;
  flex: 1;
  min-width: 200px;

  i {
    color: var(--ui-accent);
    font-size: 1rem;
  }
}

@media (max-width: 575.98px) {
  .audio-video-tabs .nav-item {
    flex: 1 1 0;
  }

  .audio-video-tabs .nav-link {
    width: 100%;
    padding-inline: 0.75rem;
    text-align: center;
  }

  .stream-mic-helper {
    align-items: stretch;
    gap: 0.75rem;
  }

  .stream-mic-download-btn,
  .stream-mic-test-btn {
    width: 100%;
  }
}

</style>
