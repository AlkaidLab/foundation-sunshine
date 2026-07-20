<script setup>
import { ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { $tp } from '../../platform-i18n'
import { openExternalUrl } from '../../utils/helpers.js'
import PlatformLayout from '../../components/layout/PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import NewDisplayOutputSelector from './audiovideo/NewDisplayOutputSelector.vue'
import LegacyDisplayOutputSelector from './audiovideo/LegacyDisplayOutputSelector.vue'
import DisplayDeviceOptions from './audiovideo/DisplayDeviceOptions.vue'
import ExperimentalFeatures from './audiovideo/ExperimentalFeatures.vue'
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

    <DisplayDeviceOptions :platform="platform" :config="config" />

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

    <ExperimentalFeatures :platform="platform" :config="config" :display_mode_remapping="display_mode_remapping" />

    <!-- 下载确认对话框 -->
    <Teleport to="body">
      <Transition name="fade">
        <div v-if="showDownloadConfirm" class="download-confirm-overlay" @click.self="cancelDownload">
          <div class="download-confirm-modal">
            <div class="download-confirm-header">
              <h5>
                <i class="fas fa-external-link-alt me-2"></i>{{ $t('_common.download') }}
              </h5>
              <button class="btn-close" :aria-label="$t('_common.close') || '关闭'" @click="cancelDownload"></button>
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
    </Teleport>
  </div>
</template>

<style scoped>
.nav-tabs {
  gap: 0.25rem;
  padding: 0.3rem;
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  background: var(--ui-surface);
  margin-bottom: 1rem;
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
  padding-top: 1rem;
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

.stream-mic-download-btn {
  white-space: nowrap;
  flex-shrink: 0;
  order: -1;
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

  .stream-mic-download-btn {
    width: 100%;
  }
}

/* Download Confirm Modal - teleported to body, styles must not be scoped */
</style>

<style>
.download-confirm-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  width: 100vw;
  height: 100vh;
  margin: 0;
  background: var(--ui-overlay);
  backdrop-filter: blur(8px);
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 1.5rem);
  overflow: hidden;
}

.download-confirm-modal {
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-lg);
  width: 100%;
  max-width: 500px;
  display: flex;
  flex-direction: column;
  backdrop-filter: blur(20px);
  color: var(--ui-text-primary);
  box-shadow: var(--ui-shadow-md);
  animation: modalSlideUp 0.3s ease;
}

.download-confirm-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1.25rem 1.5rem;
  border-bottom: 1px solid var(--ui-border);

  h5 {
    margin: 0;
    font-size: 1.125rem;
    font-weight: 600;
    color: var(--ui-text-primary);
    display: flex;
    align-items: center;

    i {
      color: var(--ui-accent);
    }
  }

  .btn-close {
    background: none;
    border: none;
    font-size: 1.5rem;
    color: var(--ui-text-secondary);
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
  color: var(--ui-text-secondary);

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
  border-top: 1px solid var(--ui-border);
  background: color-mix(in srgb, var(--ui-surface) 70%, transparent);
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

@media (max-width: 575.98px) {
  .download-confirm-overlay {
    align-items: flex-end;
    padding: 0.75rem;
  }

  .download-confirm-modal {
    border-radius: var(--ui-radius-md);
  }

  .download-confirm-header,
  .download-confirm-body,
  .download-confirm-footer {
    padding: 1rem;
  }
}

</style>
