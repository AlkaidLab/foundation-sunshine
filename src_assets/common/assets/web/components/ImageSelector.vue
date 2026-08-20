<template>
  <div class="form-group-enhanced">
    <label for="appImagePath" class="form-label-enhanced">{{ $t('apps.image') }}</label>

    <!-- 使用桌面图片选项 -->
    <div class="form-check mb-3">
      <input
        type="checkbox"
        class="form-check-input"
        id="useDesktopImage"
        :checked="isDesktopImage"
        @change="handleDesktopImageChange"
      />
      <label for="useDesktopImage" class="form-check-label">{{ $t('apps.use_desktop_image') }}</label>
    </div>

    <!-- 图片路径输入 -->
    <div
      v-if="!isDesktopImage"
      class="input-group image-input-group"
      :class="{ 'is-dragging': dragCounter > 0 }"
    >
      <input
        type="file"
        class="form-control image-file-input"
        @change="handleFileSelect"
        accept="image/png,image/jpg,image/jpeg,image/gif,image/bmp,image/webp"
      />
      <input
        type="text"
        class="form-control form-control-enhanced monospace"
        id="appImagePath"
        :value="imagePath"
        @input="updateImagePath"
        @dragenter="handleDragEnter"
        @dragleave="handleDragLeave"
        @dragover.prevent
        @drop.prevent.stop="handleDrop"
        placeholder="选择图片文件或拖拽到此处"
      />
      <button
        class="btn btn-outline-secondary"
        type="button"
        @click="openCoverFinder"
        :disabled="!appName"
      >
        <i class="fas fa-search me-1"></i>{{ $t('apps.find_cover') }}
      </button>
    </div>

    <!-- 图片预览 -->
    <div v-if="!isDesktopImage && imagePath" class="image-preview-container mt-3">
      <div class="image-preview">
        <img :src="previewUrl" alt="图片预览" @error="handleImageError" />
      </div>
      <div class="image-preview-circle">
        <img :src="previewUrl" alt="图片预览" @error="handleImageError" />
      </div>
    </div>

    <div class="field-hint">{{ $t('apps.image_desc') }}</div>

    <!-- 封面查找器 -->
    <CoverFinder
      :visible="showCoverFinder"
      :search-term="appName"
      @close="closeCoverFinder"
      @cover-selected="handleCoverSelected"
      @loading="handleCoverLoading"
      @error="handleCoverError"
    />
  </div>
</template>

<script>
import CoverFinder from './CoverFinder.vue'
import { validateFile } from '../utils/validation.js'
import { getImagePreviewUrl } from '../utils/imageUtils.js'
import { apiPostJson } from '../utils/apiFetch.js'

export default {
  name: 'ImageSelector',
  components: {
    CoverFinder,
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
  },
  data() {
    return {
      showCoverFinder: false,
      coverLoading: false,
      dragCounter: 0,
    }
  },
  computed: {
    isDesktopImage() {
      return this.imagePath === 'desktop'
    },
    previewUrl() {
      return getImagePreviewUrl(this.imagePath)
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
      if (!file) return

      await this.processFile(file)
    },

    /**
     * 处理拖拽进入
     */
    handleDragEnter(event) {
      event.preventDefault()
      this.dragCounter++
      this.$emit('image-error', '杂鱼~快放进来呀~')
    },

    /**
     * 处理拖拽离开
     */
    handleDragLeave(event) {
      event.preventDefault()
      this.dragCounter--
      if (this.dragCounter === 0) {
        this.$emit('image-error', '')
      }
    },

    /**
     * 处理拖拽放置
     */
    async handleDrop(event) {
      event.preventDefault()
      this.dragCounter = 0

      const file = event.dataTransfer.files[0]
      if (!file) {
        this.$emit('image-error', '其他地方不可以！')
        return
      }

      await this.processFile(file)
    },

    /**
     * 处理文件上传
     */
    async processFile(file) {
      const validation = validateFile(file)
      if (!validation.isValid) {
        this.$emit('image-error', validation.message)
        return
      }

      try {
        this.$emit('image-error', '正在上传图片...')
        const path = await this.uploadImageToSunshine(file)
        this.$emit('update-image', path)
        this.$emit('image-error', '')
      } catch (error) {
        console.error('上传图片失败:', error)
        this.$emit('image-error', `上传图片失败: ${error.message}`)
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
      return `app_${appName}_${timestamp}`.replace(/[^a-zA-Z0-9_-]/g, '_')
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
      this.$emit('image-error', '图片加载失败，请检查文件路径')
    },

    /**
     * 打开封面查找器
     */
    openCoverFinder() {
      if (!this.appName) {
        this.$emit('image-error', '请先输入应用名称')
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
.monospace {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
}

.image-file-input {
  flex: 0 0 132px !important;
  max-width: 132px;
}

.image-input-group .form-control[type='file'] {
  border-top-right-radius: 0;
  border-bottom-right-radius: 0;
}

.image-input-group .form-control:not([type='file']) {
  border-right: 0;
  border-left: 0;
  border-radius: 0;
}

.image-input-group .btn {
  border-top-left-radius: 0;
  border-bottom-left-radius: 0;
}

.image-input-group.is-dragging .form-control-enhanced {
  background: var(--ui-accent-soft);
  border-color: var(--ui-accent);
  box-shadow: inset 0 0 0 1px var(--ui-accent);
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.6;
}

.image-preview-container {
  display: flex;
  align-items: center;
  justify-content: flex-start;
  gap: 1rem;
  flex-wrap: wrap;
}

.image-preview {
  display: flex;
  align-items: center;
  justify-content: center;
  width: min(100%, 320px);
  min-height: 172px;
  padding: 1rem;
  text-align: center;
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
}

.image-preview img {
  max-width: 100%;
  max-height: 148px;
  object-fit: contain;
  border-radius: var(--ui-radius-sm);
  box-shadow: var(--ui-shadow-sm);
}

.image-preview-circle {
  position: relative;
  width: 132px;
  height: 132px;
  padding: 3px;
  overflow: hidden;
  text-align: center;
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: 50%;
  box-shadow: var(--ui-shadow-sm);
}

.image-preview-circle img {
  position: absolute;
  top: 50%;
  left: 50%;
  width: calc(100% - 6px);
  height: calc(100% - 6px);
  object-fit: cover;
  border-radius: 50%;
  transform: translate(-50%, -50%);
}

.image-preview-circle::after {
  position: absolute;
  top: 50%;
  left: 50%;
  width: 16%;
  height: 16%;
  content: '';
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border-strong);
  border-radius: 50%;
  transform: translate(-50%, -50%);
}

@media (max-width: 768px) {
  .image-input-group {
    flex-direction: column;
    gap: 0.5rem;
  }

  .image-input-group .form-control,
  .image-input-group .btn,
  .image-file-input {
    width: 100% !important;
    max-width: none;
    margin: 0;
    border: 1px solid var(--ui-border) !important;
    border-radius: var(--ui-radius-sm) !important;
  }

  .image-preview-container {
    align-items: stretch;
    flex-direction: column;
  }

  .image-preview {
    width: 100%;
  }

  .image-preview-circle {
    align-self: center;
  }
}
</style>
