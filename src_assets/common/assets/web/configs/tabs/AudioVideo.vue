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
import ConfirmDialog from '../../components/common/ConfirmDialog.vue'

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

</style>
