<template>
  <section class="cover-card" :class="[`cover-card--${kind}`, { 'is-dragging': dragCounter > 0 }]" :aria-label="title" @dragenter="handleDragEnter" @dragleave="handleDragLeave" @dragover.prevent @drop.prevent.stop="handleDrop">
    <header v-if="title" class="cover-card__head">
      <h3 class="cover-card__title">{{ title }}</h3>
      <MChip>{{ ratioLabel }}</MChip>
    </header>
    <MGroup>
      <div class="m-item cover-stage">
        <div class="cover-stage__row">
          <div class="cover-frame" :class="{ 'is-empty': !hasPicture, 'is-broken': showBroken }">
            <div v-if="isDesktopImage" class="cover-frame__state"><MIcon name="wallpaper" fill /><span>{{ $t('apps.use_desktop_image') }}</span></div>
            <img v-else-if="imagePath && !broken" class="image-preview" :class="`image-preview--${kind}`" :src="previewUrl" :alt="$t('apps.image_preview')" @error="handleImageError" />
            <div v-else-if="showBroken" class="cover-frame__state cover-frame__state--error" role="alert"><MIcon name="error" fill /><span>{{ $t('apps.image_load_failed') }}</span></div>
            <button v-else type="button" class="cover-frame__state cover-frame__choose" @click="pickFile"><MIcon name="image" fill /><span>{{ $t('apps.choose_image') }}</span></button>
            <CoverBadge v-if="badge && hasPicture && !isDesktopImage" :kind="kind" />
            <div v-if="dragCounter > 0" class="cover-frame__scrim"><MIcon name="upload" fill /><span>{{ $t('apps.image_drop_hint') }}</span></div>
            <div v-else-if="uploading" class="cover-frame__scrim" role="status" aria-live="polite"><MLoading /><span>{{ $t('apps.image_uploading') }}</span></div>
          </div>
          <div v-if="kind === 'poster' && imagePath && !isDesktopImage && !broken" class="image-preview-circle" aria-hidden="true">
            <img :src="previewUrl" alt="" @error="handleImageError" />
          </div>
        </div>
        <div v-if="!isDesktopImage" class="cover-actions">
          <input ref="fileInput" type="file" class="image-file-input" tabindex="-1" aria-hidden="true" accept="image/png,image/jpg,image/jpeg,image/gif,image/bmp,image/webp" @change="handleFileSelect" />
          <MButton variant="tonal" icon="upload" :loading="uploading" @click="pickFile">{{ $t('apps.choose_image') }}</MButton>
          <MButton v-if="kind === 'poster'" variant="tonal" icon="search" @click="openCoverFinder">{{ $t('apps.find_cover') }}</MButton>
          <MButton v-if="imagePath" variant="text" icon="close" data-image="clear" @click="$emit('update-image', '')">{{ $t('apps.image_clear') }}</MButton>
        </div>
      </div>
      <MSwitchItem v-if="kind === 'poster'" :model-value="isDesktopImage" icon="wallpaper" :title="$t('apps.use_desktop_image')" @update:model-value="(on) => $emit('update-image', on ? 'desktop' : '')" />
      <div v-if="!isDesktopImage" class="m-item m-item--field image-path">
        <MField :model-value="imagePath" :label="$t('apps.image')" :placeholder="$t('apps.image_path_placeholder')" :supporting="kind === 'poster' ? $t('apps.image_desc') : ''" @update:model-value="(value) => $emit('update-image', value)" />
      </div>
    </MGroup>

    <CoverFinder
      v-if="kind === 'poster'"
      :visible="showCoverFinder"
      :search-term="appName"
      @close="closeCoverFinder"
      @cover-selected="handleCoverSelected"
      @loading="handleCoverLoading"
      @error="handleCoverError"
    />
  </section>
</template>

<script>
import { MButton, MChip, MField, MGroup, MIcon, MIconButton, MLoading, MSwitchItem } from 'foundation-ui/vue'
import CoverFinder from './CoverFinder.vue'
import CoverBadge from './sparxie/CoverBadge.vue'
import { validateFile } from '../utils/validation.js'
import { getImagePreviewUrl } from '../utils/imageUtils.js'
import { apiPostJson } from '../utils/apiFetch.js'

export default {
  name: 'ImageSelector',
  components: {
    CoverBadge,
    CoverFinder,
    MButton,
    MChip,
    MField,
    MGroup,
    MIcon,
    MIconButton,
    MLoading,
    MSwitchItem,
  },
  props: {
    imagePath: {
      type: String,
      default: '',
    },
    appName: {
      type: String,
      default: '',
    },
    // 'poster' is the portrait cover (image-path: stock Moonlight shows it); 'hero' is the landscape cover (served to clients as /appasset?type=hero)
    kind: {
      type: String,
      default: 'poster',
      validator: (value) => value === 'poster' || value === 'hero',
    },
    title: {
      type: String,
      default: '',
    },
    // the cover carries the Sparxie mark where the host draws it (a Sparxie application with the badge on): shown on the preview
    badge: {
      type: Boolean,
      default: false,
    },
  },
  data() {
    return {
      showCoverFinder: false,
      coverLoading: false,
      dragCounter: 0,
      uploading: false,
      broken: false,
    }
  },
  computed: {
    isDesktopImage() {
      return this.imagePath === 'desktop'
    },
    previewUrl() {
      return getImagePreviewUrl(this.imagePath)
    },
    ratioLabel() {
      return this.kind === 'hero' ? '16:9' : '2:3'
    },
    hasPicture() {
      return this.isDesktopImage || (!!this.imagePath && !this.broken)
    },
    showBroken() {
      return !this.isDesktopImage && !!this.imagePath && this.broken
    },
  },
  watch: {
    // a new path is a new picture: it has not failed yet
    imagePath() {
      this.broken = false
    },
  },
  methods: {
    /**
     * 处理桌面图片选择变化
     */
    handleDesktopImageChange(event) {
      this.$emit('update-image', event.target.checked ? 'desktop' : '')
    },

    /**
     * 更新图片路径
     */
    updateImagePath(event) {
      this.$emit('update-image', event.target.value)
    },

    /**
     * 处理文件选择
     */
    async handleFileSelect(event) {
      const file = event.target.files[0]
      event.target.value = ''
      if (!file) return

      await this.processFile(file)
    },

    /**
     * 处理拖拽进入
     */
    handleDragEnter(event) {
      event.preventDefault()
      this.dragCounter++
    },

    /**
     * 处理拖拽离开
     */
    handleDragLeave(event) {
      event.preventDefault()
      this.dragCounter = Math.max(0, this.dragCounter - 1)
    },

    /**
     * 处理拖拽放置
     */
    async handleDrop(event) {
      event.preventDefault()
      this.dragCounter = 0

      const file = event.dataTransfer.files[0]
      if (!file) {
        this.$emit('image-error', this.$t('apps.image_drop_empty'))
        return
      }

      await this.processFile(file)
    },

    /**
     * 处理文件上传
     */
    async processFile(file) {
      const validation = validateFile(file, { translate: this.$t })
      if (!validation.isValid) {
        this.$emit('image-error', validation.message)
        return
      }

      this.uploading = true
      try {
        this.$emit('image-error', '')
        const path = await this.uploadImageToSunshine(file)
        this.$emit('update-image', path)
        this.$emit('image-error', '')
      } catch (error) {
        console.error('上传图片失败:', error)
        this.$emit('image-error', this.$t('apps.image_upload_failed', { error: error.message }))
      } finally {
        this.uploading = false
      }
    },

    /**
     * 上传图片到 Sunshine API
     */
    async uploadImageToSunshine(file) {
      const base64Data = await this.readFileAsBase64(file)
      const key = this.generateImageKey()

      const result = await apiPostJson('/api/covers/upload', { key, data: base64Data })
      console.log('✅ Sunshine API 上传成功，文件路径:', result.path)

      return `${key}.png`
    },

    /**
     * 读取文件为 Base64
     */
    readFileAsBase64(file) {
      return new Promise((resolve, reject) => {
        const reader = new FileReader()
        reader.onload = () => resolve(reader.result.split(',')[1])
        reader.onerror = reject
        reader.readAsDataURL(file)
      })
    },

    /**
     * 生成图片 key
     */
    generateImageKey() {
      const timestamp = Date.now()
      const appName = this.appName || 'custom'
      const prefix = this.kind === 'hero' ? 'app_hero' : 'app'
      return `${prefix}_${appName}_${timestamp}`.replace(/[^a-zA-Z0-9_-]/g, '_')
    },

    /**
     * 获取图片预览URL
     */
    getImagePreviewUrl() {
      return getImagePreviewUrl(this.imagePath)
    },

    /**
     * 处理图片加载错误
     */
    handleImageError() {
      this.broken = true
      this.$emit('image-error', this.$t('apps.image_load_failed'))
    },

    /** 打开文件选择 */
    pickFile() {
      this.$refs.fileInput?.click()
    },

    /**
     * 打开封面查找器
     */
    openCoverFinder() {
      if (!this.appName) {
        this.$emit('image-error', this.$t('apps.image_app_name_required'))
        return
      }
      this.showCoverFinder = true
    },

    /**
     * 关闭封面查找器
     */
    closeCoverFinder() {
      this.showCoverFinder = false
    },

    /**
     * 处理封面选择
     */
    handleCoverSelected(coverData) {
      this.$emit('update-image', coverData.path)
      this.showCoverFinder = false
    },

    /**
     * 处理封面加载状态
     */
    handleCoverLoading(loading) {
      this.coverLoading = loading
    },

    /**
     * 处理封面错误
     */
    handleCoverError(error) {
      this.$emit('image-error', error)
    },
  },
}
</script>

<style scoped>
.cover-card {
  display: flex;
  flex-direction: column;
  gap: 8px;
  min-width: 0;
}

.cover-card__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 0 16px;
}

.cover-card__title {
  margin: 0;
  font: var(--m-title-small);
  color: var(--md-sys-color-on-surface-variant);
}

.cover-stage {
  flex-direction: column;
  align-items: stretch;
  gap: 16px;
  padding: 16px;
}

.cover-stage__row {
  display: flex;
  align-items: flex-end;
  gap: 16px;
}

/* the frame has the shape of the cover: 2:3 for the portrait one, 16:9 for the landscape one */
.cover-frame {
  position: relative;
  flex: 0 1 auto;
  min-width: 0;
  overflow: hidden;
  background: var(--md-sys-color-surface-container-highest);
  border-radius: var(--md-sys-shape-corner-large);
}

.cover-card--poster .cover-frame {
  width: min(100%, 176px);
  aspect-ratio: 2 / 3;
}

.cover-card--hero .cover-frame {
  width: 100%;
  aspect-ratio: 16 / 9;
}

.cover-frame.is-broken {
  background: var(--md-sys-color-error-container);
  color: var(--md-sys-color-on-error-container);
}

.image-preview {
  display: block;
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.cover-frame__state {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 12px;
  text-align: center;
  font: var(--m-label-large);
  color: var(--md-sys-color-on-surface-variant);
}

.cover-frame.is-broken .cover-frame__state {
  color: inherit;
}

.cover-frame__state .m-icon {
  font-size: 32px;
}

button.cover-frame__choose {
  border: 0;
  background: transparent;
  cursor: pointer;
  font-family: inherit;
}

button.cover-frame__choose:focus-visible {
  outline: 2px solid var(--md-sys-color-primary);
  outline-offset: -4px;
  border-radius: inherit;
}

.cover-frame__scrim {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 12px;
  text-align: center;
  font: var(--m-label-large);
  color: var(--md-sys-color-on-primary-container);
  background: var(--md-sys-color-primary-container);
}

.cover-card.is-dragging .cover-stage {
  box-shadow: inset 0 0 0 2px var(--md-sys-color-primary);
}

.cover-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
}

.image-file-input {
  display: none;
}

/* how the cover is cut where the client draws it round */
.image-preview-circle {
  position: relative;
  flex: none;
  width: 72px;
  height: 72px;
  overflow: hidden;
  background: var(--md-sys-color-surface-container-highest);
  border-radius: 50%;
}

.image-preview-circle img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
</style>
