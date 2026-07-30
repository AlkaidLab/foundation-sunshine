<script setup>
import { ref } from 'vue'
import PlatformLayout from '../../../components/layout/PlatformLayout.vue'
import VulkanHdrBridgeCard from './VulkanHdrBridgeCard.vue'

const props = defineProps({
  platform: String,
  config: Object,
  resolutions: Array,
  fps: Array,
})

const resolutions = ref(props.resolutions)
const fps = ref(props.fps)
const config = ref(props.config)

const resIn = ref('')
const fpsIn = ref('')

const MAX_RESOLUTIONS = 25
const MAX_FPS = 5
const MIN_FPS_VALUE = 1
const MAX_FPS_VALUE = 480

function addResolution() {
  if (resIn.value && resolutions.value.length < MAX_RESOLUTIONS) {
    resolutions.value.push(resIn.value)
    resIn.value = ''
  }
}

function removeResolution(index) {
  resolutions.value.splice(index, 1)
}

function validateFps(value) {
  // 支持整数和小数格式（如 60, 59.94, 29.97）
  const pattern = /^\d+(\.\d+)?$/
  if (!pattern.test(value)) {
    return false
  }
  const fpsValue = parseFloat(value)
  return fpsValue >= MIN_FPS_VALUE && fpsValue <= MAX_FPS_VALUE
}

function addFps() {
  const value = fpsIn.value.trim()
  if (!value) {
    fpsIn.value = ''
    return
  }
  
  if (!validateFps(value)) {
    // 验证失败，清空输入但不显示错误（由HTML5 pattern验证处理）
    fpsIn.value = ''
    return
  }
  
  const fpsValue = parseFloat(value)
  if (fpsValue >= MIN_FPS_VALUE && fpsValue <= MAX_FPS_VALUE && fps.value.length < MAX_FPS) {
    if (!fps.value.includes(value)) {
      fps.value.push(value)
    }
  }
  fpsIn.value = ''
}

function removeFps(index) {
  fps.value.splice(index, 1)
}
</script>

<template>
  <div class="virtual-display-settings">
    <div class="form-text description-text">
      <i class="fas fa-info-circle"></i>
      {{ $t('config.res_fps_desc') }}
    </div>

    <!-- Advertised Resolutions -->
    <div class="settings-section">
      <div class="section-header">
        <i class="fas fa-desktop section-icon"></i>
        <label class="section-title">{{ $t('config.resolutions') }}</label>
      </div>
      <div class="tags-container">
        <transition-group name="tag-fade" tag="div" class="tags-wrapper">
          <div class="tag-item" v-for="(r, i) in resolutions" :key="r + i">
            <span class="tag-text">{{ r }}</span>
            <button class="tag-remove" @click="removeResolution(i)" :title="$t('_common.remove')">
              <i class="fas fa-times"></i>
            </button>
          </div>
        </transition-group>
        <div v-if="resolutions.length === 0" class="empty-hint">
          {{ $t('config.no_resolutions') || 'No resolutions added' }}
        </div>
      </div>
      <form @submit.prevent="addResolution" class="add-form">
        <input
          type="text"
          v-model="resIn"
          required
          pattern="[0-9]+x[0-9]+"
          class="form-control add-input"
          placeholder="1920x1080"
        />
        <button v-if="resolutions.length < MAX_RESOLUTIONS" class="btn btn-primary add-btn" type="submit">
          <i class="fas fa-plus"></i>
        </button>
      </form>
      <div class="limit-hint" v-if="resolutions.length >= MAX_RESOLUTIONS">
        {{ $t('config.max_resolutions_reached') || 'Maximum resolutions reached' }}
      </div>
    </div>

    <!-- Advertised FPS -->
    <div class="settings-section">
      <div class="section-header">
        <i class="fas fa-tachometer-alt section-icon"></i>
        <label class="section-title">{{ $t('config.fps') }}</label>
      </div>
      <div class="tags-container">
        <transition-group name="tag-fade" tag="div" class="tags-wrapper">
          <div class="tag-item tag-fps" v-for="(f, i) in fps" :key="f + i">
            <span class="tag-text">{{ f }} FPS</span>
            <button class="tag-remove" @click="removeFps(i)" :title="$t('_common.remove')">
              <i class="fas fa-times"></i>
            </button>
          </div>
        </transition-group>
        <div v-if="fps.length === 0" class="empty-hint">
          {{ $t('config.no_fps') || 'No FPS values added' }}
        </div>
      </div>
      <form @submit.prevent="addFps" class="add-form">
        <input
          type="text"
          v-model="fpsIn"
          required
          pattern="\d+(\.\d+)?"
          class="form-control add-input add-input-fps"
          placeholder="例如: 120 或 119.88"
        />
        <button v-if="fps.length < MAX_FPS" class="btn btn-primary add-btn" type="submit">
          <i class="fas fa-plus"></i>
        </button>
      </form>
      <div class="limit-hint" v-if="fps.length >= MAX_FPS">
        {{ $t('config.max_fps_reached') || 'Maximum FPS values reached' }}
      </div>
    </div>

    <PlatformLayout :platform="platform">
      <template #windows>
        <div class="settings-section">
          <div class="section-header">
            <i class="fas fa-sliders-h section-icon"></i>
            <label class="section-title">{{ $t('tabs.advanced') }}</label>
          </div>

          <div class="form-check form-switch">
            <input
              id="vdd_borrowed_texture"
              v-model="config.vdd_borrowed_texture"
              class="form-check-input"
              type="checkbox"
              role="switch"
              aria-describedby="vdd_borrowed_texture_desc"
              true-value="enabled"
              false-value="disabled"
            />
            <label class="form-check-label" for="vdd_borrowed_texture">
              {{ $t('config.vdd_borrowed_texture') }}
            </label>
          </div>
          <div id="vdd_borrowed_texture_desc" class="form-text">
            {{ $t('config.vdd_borrowed_texture_desc') }}
          </div>

          <VulkanHdrBridgeCard class="mt-3" :config="config" />
        </div>
      </template>
    </PlatformLayout>

  </div>
</template>

<style scoped>
.virtual-display-settings {
  padding: 0;
}

.settings-section {
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  padding: 1rem;
  margin-bottom: 1rem;
  box-shadow: var(--ui-shadow-sm);
}

.section-header {
  display: flex;
  align-items: center;
  margin-bottom: 0.75rem;
}

.section-icon {
  color: var(--ui-accent);
  margin-right: 0.5rem;
  font-size: 1rem;
}

.section-title {
  font-weight: 600;
  font-size: 0.95rem;
  color: var(--ui-text-primary);
  margin: 0;
}

.tags-container {
  min-height: 40px;
  margin-bottom: 0.75rem;
}

.tags-wrapper {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
}

.tag-item {
  display: inline-flex;
  align-items: center;
  background: var(--ui-accent-soft);
  color: var(--ui-accent);
  border: 1px solid var(--ui-border-strong);
  padding: 0.35rem 0.5rem 0.35rem 0.75rem;
  border-radius: 999px;
  font-size: 0.85rem;
  font-weight: 500;
  transition: transform 0.2s ease, box-shadow 0.2s ease, background-color 0.2s ease;
}

.tag-item:hover {
  transform: translateY(-1px);
  background: var(--ui-surface-hover);
  box-shadow: var(--ui-shadow-sm);
}

.tag-fps {
  border-color: color-mix(in srgb, var(--ui-success) 34%, transparent);
  background: color-mix(in srgb, var(--ui-success) 12%, transparent);
  color: var(--ui-success-text);
}

.tag-text {
  margin-right: 0.5rem;
  font-family: 'SF Mono', 'Monaco', 'Inconsolata', 'Roboto Mono', monospace;
}

.tag-remove {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 20px;
  height: 20px;
  border: 1px solid currentColor;
  background: var(--ui-surface-strong);
  color: inherit;
  border-radius: 50%;
  cursor: pointer;
  transition: all 0.2s ease;
  padding: 0;
  font-size: 0.7rem;
}

.tag-remove:hover {
  background: var(--ui-surface-hover);
  transform: scale(1.1);
}

.tag-remove:focus-visible {
  outline: 2px solid currentColor;
  outline-offset: 2px;
}

.empty-hint {
  color: var(--ui-text-muted);
  font-size: 0.85rem;
  font-style: italic;
  padding: 0.5rem 0;
}

.add-form {
  display: flex;
  align-items: center;
  gap: 0;
}

.add-input {
  width: 140px;
  border-top-right-radius: 0;
  border-bottom-right-radius: 0;
  font-family: 'SF Mono', 'Monaco', 'Inconsolata', 'Roboto Mono', monospace;
}

.add-input-fps {
  width: 80px;
}

.add-btn {
  border-top-left-radius: 0;
  border-bottom-left-radius: 0;
  padding: 0.375rem 0.75rem;
}

.limit-hint {
  color: var(--ui-warning-text);
  font-size: 0.8rem;
  margin-top: 0.5rem;
}

.description-text {
  display: flex;
  align-items: flex-start;
  gap: 0.5rem;
  padding: 0.75rem;
  background: var(--ui-accent-soft);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  color: var(--ui-text-secondary);
  margin-bottom: 1rem;
}

.description-text i {
  color: var(--ui-accent);
  margin-top: 0.15rem;
}

/* Transition animations */
.tag-fade-enter-active,
.tag-fade-leave-active {
  transition: all 0.3s ease;
}

.tag-fade-enter-from,
.tag-fade-leave-to {
  opacity: 0;
  transform: scale(0.8);
}

@media (max-width: 575.98px) {
  .add-form {
    width: 100%;
  }

  .add-input,
  .add-input-fps {
    width: auto;
    min-width: 0;
    flex: 1;
  }
}
</style>
