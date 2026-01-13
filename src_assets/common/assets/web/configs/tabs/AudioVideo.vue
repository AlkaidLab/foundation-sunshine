<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../platform-i18n'
import { openExternalUrl } from '../../utils/helpers.js'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import LegacyDisplayOutputSelector from './audiovideo/LegacyDisplayOutputSelector.vue'
import NewDisplayOutputSelector from './audiovideo/NewDisplayOutputSelector.vue'
import DisplayDeviceOptions from './audiovideo/DisplayDeviceOptions.vue'
import DisplayModesSettings from './audiovideo/DisplayModesSettings.vue'
import VirtualDisplaySettings from './audiovideo/VirtualDisplaySettings.vue'
import Checkbox from '../../components/Checkbox.vue'

const props = defineProps(['platform', 'config', 'resolutions', 'fps', 'display_mode_remapping', 'min_fps_factor'])

const { t } = useI18n()
const config = ref(props.config)
const currentSubTab = ref('display-modes')
const showDownloadConfirm = ref(false)

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
      <div class="stream-mic-helper mt-2">
        <button
          type="button"
          class="btn btn-sm btn-primary stream-mic-download-btn"
          @click="handleDownloadVSink"
        >
          <i class="fas fa-download me-1"></i>
          {{ $t('_common.download') }}
        </button>
        <div class="stream-mic-note">
          <i class="fas fa-info-circle me-2"></i>
          <span>{{ $t('config.stream_mic_note') }}</span>
        </div>
      </div>
    </div>

    <AdapterNameSelector :platform="platform" :config="config" />

    <NewDisplayOutputSelector :platform="platform" :config="config" />

    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Capture Target -->
        <div class="mb-3">
          <label for="capture_target" class="form-label">{{ $t('config.capture_target') }}</label>
          <select id="capture_target" class="form-select" v-model="config.capture_target">
            <option value="display">{{ $t('config.capture_target_display') }}</option>
            <option value="window">{{ $t('config.capture_target_window') }}</option>
          </select>
          <div class="form-text">{{ $t('config.capture_target_desc') }}</div>
        </div>

        <!-- Window Title (only shown when capture_target is window) -->
        <div class="mb-3" v-if="config.capture_target === 'window'">
          <label for="window_title" class="form-label">{{ $t('config.window_title') }}</label>
          <input
            type="text"
            class="form-control"
            id="window_title"
            :placeholder="$t('config.window_title_placeholder')"
            v-model="config.window_title"
          />
          <div class="form-text">{{ $t('config.window_title_desc') }}</div>
        </div>
      </template>
    </PlatformLayout>

    <DisplayDeviceOptions :platform="platform" :config="config" :display_mode_remapping="display_mode_remapping" />

    <!-- Display Modes Tab Navigation -->
    <div class="mb-3">
      <ul class="nav nav-tabs">
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
          :platform="platform"
          :config="config"
          :min_fps_factor="min_fps_factor"
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

    <!-- 下载确认对话框 -->
    <Transition name="fade">
      <div v-if="showDownloadConfirm" class="download-confirm-overlay" @click.self="cancelDownload">
        <div class="download-confirm-modal">
          <div class="download-confirm-header">
            <h5>
              <i class="fas fa-external-link-alt me-2"></i>{{ $t('_common.download') }}
            </h5>
            <button class="btn-close" @click="cancelDownload"></button>
          </div>
          <div class="download-confirm-body">
            <p>{{ $t('config.stream_mic_download_confirm') }}</p>
          </div>
          <div class="download-confirm-footer">
            <button type="button" class="btn btn-secondary" @click="cancelDownload">{{ $t('_common.cancel') }}</button>
            <button type="button" class="btn btn-primary" @click="confirmDownload">
              <i class="fas fa-download me-1"></i>{{ $t('_common.download') }}
            </button>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.nav-tabs {
  border-bottom: 1px solid rgba(0, 0, 0, 0.1);
  margin-bottom: 1rem;
}

.nav-tabs .nav-link {
  border: none;
  border-bottom: 2px solid transparent;
  color: var(--bs-secondary-color);
  padding: 0.75rem 1.5rem;
  transition: all 0.3s ease;
}

.nav-tabs .nav-link:hover {
  border-bottom-color: var(--bs-primary);
  color: var(--bs-primary);
}

.nav-tabs .nav-link.active {
  color: var(--bs-primary);
  background-color: transparent;
  border-bottom-color: var(--bs-primary);
  font-weight: 600;
}

.tab-content {
  padding-top: 1rem;
}

.stream-mic-helper {
  display: flex;
  align-items: center;
  gap: 1rem;
  flex-wrap: wrap;
  padding: 0.75rem;
  background: var(--bs-secondary-bg);
  border-radius: 8px;
  border: 1px solid var(--bs-border-color);
}

.stream-mic-download-btn {
  white-space: nowrap;
  flex-shrink: 0;
  order: -1;
}

.stream-mic-note {
  display: flex;
  align-items: center;
  color: var(--bs-secondary-color);
  font-size: 0.875rem;
  flex: 1;
  min-width: 200px;

  i {
    color: var(--bs-info);
    font-size: 1rem;
  }
}

[data-bs-theme='dark'] .stream-mic-helper {
  background: rgba(255, 255, 255, 0.05);
  border-color: rgba(255, 255, 255, 0.1);
}

/* Download Confirm Modal */
.download-confirm-overlay {
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

.download-confirm-modal {
  background: var(--modal-bg, rgba(30, 30, 50, 0.95));
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.2));
  border-radius: var(--border-radius-xl, 12px);
  width: 100%;
  max-width: 500px;
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

.download-confirm-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1.25rem 1.5rem;
  border-bottom: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  [data-bs-theme='light'] & {
    border-bottom-color: rgba(0, 0, 0, 0.1);
  }

  h5 {
    margin: 0;
    font-size: 1.125rem;
    font-weight: 600;
    color: var(--bs-body-color);
    display: flex;
    align-items: center;

    i {
      color: var(--bs-primary);
    }
  }

  .btn-close {
    background: none;
    border: none;
    font-size: 1.5rem;
    color: var(--bs-secondary-color);
    cursor: pointer;
    padding: 0;
    width: 1.5rem;
    height: 1.5rem;
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0.6;
    transition: opacity 0.2s;

    &:hover {
      opacity: 1;
    }

    &::before {
      content: '×';
    }
  }
}

.download-confirm-body {
  padding: 1.5rem;
  color: var(--bs-body-color);

  p {
    margin: 0;
    line-height: 1.6;
  }
}

.download-confirm-footer {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 0.75rem;
  padding: 1.25rem 1.5rem;
  border-top: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  [data-bs-theme='light'] & {
    border-top-color: rgba(0, 0, 0, 0.1);
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

.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

</style>
