<template>
  <div class="app-card" :class="{ 'app-card-dragging': isDragging }">
    <div class="app-card-inner">
      <!-- 应用图标 -->
      <div class="app-icon-container">
        <img 
          v-if="app['image-path']" 
          :src="getImageUrl()" 
          :alt="app.name"
          class="app-icon"
          loading="lazy"
          decoding="async"
          @error="handleImageError"
        >
        <div v-else class="app-icon-placeholder">
          <i class="fas fa-desktop"></i>
        </div>
      </div>
      
      <!-- 应用信息 -->
      <div class="app-info" :title="app.cmd || undefined">
        <h3 class="app-name">{{ app.name }}</h3>
        <button
          v-if="app.cmd"
          type="button"
          class="app-command app-command-copy"
          :title="`${$t('_common.copy')}: ${app.name}`"
          :aria-label="`${$t('_common.copy')}: ${app.name}`"
          @click="copyToClipboard(app.cmd, app.name, $event)"
        >
          <i class="fas fa-terminal me-1"></i>
          <span class="app-command-text">{{ truncateText(app.cmd, 50) }}</span>
          <i class="fas fa-copy app-command-copy-icon" aria-hidden="true"></i>
        </button>
        <div v-if="hasTags" class="app-tags">
          <span
            v-for="tag in visibleTags"
            :key="tag.key"
            class="app-tag"
            :class="tag.className"
            :title="tag.title"
          >
            <span v-if="tag.count" class="badge rounded-pill bg-secondary me-1">{{ tag.count }}</span>
            <i v-else class="fas me-1" :class="tag.icon"></i>{{ tag.label }}
          </span>
          <span v-if="hiddenTagCount > 0" class="app-tag tag-more" :title="tagSummaryTitle">
            +{{ hiddenTagCount }}
          </span>
        </div>
      </div>
      
      <!-- 操作按钮 -->
      <div class="app-actions">
        <button 
          class="btn btn-edit" 
          @click="$emit('edit')"
          :title="$t('apps.edit')"
        >
          <i class="fas fa-edit"></i>
        </button>
        <button 
          class="btn btn-delete" 
          @click="$emit('delete')"
          :title="$t('apps.delete')"
        >
          <i class="fas fa-trash"></i>
        </button>
      </div>
      
      <!-- 拖拽手柄 -->
      <div v-if="draggable" class="drag-handle">
        <i class="fas fa-grip-vertical"></i>
      </div>
      
      <!-- 搜索状态指示 -->
      <div v-if="isSearchResult" class="search-indicator">
        <i class="fas fa-search"></i>
      </div>
    </div>
  </div>
</template>

<script>
import { getImagePreviewUrl } from '../utils/imageUtils.js';

export default {
  name: 'AppCard',
  props: {
    app: {
      type: Object,
      required: true
    },
    draggable: {
      type: Boolean,
      default: true
    },
    isSearchResult: {
      type: Boolean,
      default: false
    },
    isDragging: {
      type: Boolean,
      default: false
    }
  },
  emits: ['edit', 'delete', 'copy-success', 'copy-error'],
  computed: {
    tagItems() {
      const tags = [];

      if (this.app['exclude-global-prep-cmd'] && this.app['exclude-global-prep-cmd'] !== 'false') {
        tags.push({
          key: 'exclude-global-prep-cmd',
          className: 'tag-exclude-global-prep-cmd',
          icon: 'fa-ellipsis-h',
          label: '跳过预处理',
          title: '全局预处理命令',
        });
      }

      if (this.app['menu-cmd'] && this.app['menu-cmd'].length > 0) {
        tags.push({
          key: 'menu-cmd',
          className: 'tag-menu',
          count: this.app['menu-cmd'].length,
          label: '菜单',
          title: '菜单命令',
        });
      }

      if (this.app.elevated && this.app.elevated !== 'false') {
        tags.push({
          key: 'elevated',
          className: 'tag-elevated',
          icon: 'fa-shield-alt',
          label: '管理员',
          title: '管理员',
        });
      }

      if (this.app['auto-detach'] && this.app['auto-detach'] !== 'false') {
        tags.push({
          key: 'auto-detach',
          className: 'tag-detach',
          icon: 'fa-unlink',
          label: '分离运行',
          title: '关闭时不退出串流',
        });
      }

      return tags;
    },
    visibleTags() {
      return this.tagItems.slice(0, 2);
    },
    hiddenTagCount() {
      return Math.max(0, this.tagItems.length - this.visibleTags.length);
    },
    tagSummaryTitle() {
      return this.tagItems.map(tag => tag.title).join(' / ');
    },
    hasTags() {
      return this.tagItems.length > 0;
    }
  },
  methods: {
    /**
     * 处理图像错误
     */
    handleImageError(event) {
      const element = event.target;
      element.style.display = 'none';
      if (element.nextElementSibling) {
        element.nextElementSibling.style.display = 'flex';
      }
    },
    
    /**
     * 获取图片URL
     */
    getImageUrl() {
      return getImagePreviewUrl(this.app['image-path']);
    },
    
    /**
     * 截断文本
     */
    truncateText(text, length) {
      if (!text) return '';
      if (text.length <= length) return text;
      return text.substring(0, length) + '...';
    },
    
    /**
     * 复制到剪贴板
     */
    async copyToClipboard(text, appName, event) {
      if (!text) {
        this.$emit('copy-error', '没有可复制的命令');
        return;
      }
      
      const targetElement = event.currentTarget;
      
      try {
        // 使用现代的 Clipboard API
        if (navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(text);
          this.showCopySuccess(targetElement, appName);
        } else {
          // 回退方案：使用传统的 execCommand
          const textArea = document.createElement('textarea');
          textArea.value = text;
          textArea.style.position = 'fixed';
          textArea.style.left = '-999999px';
          textArea.style.top = '-999999px';
          document.body.appendChild(textArea);
          textArea.focus();
          textArea.select();
          
          try {
            document.execCommand('copy');
            this.showCopySuccess(targetElement, appName);
          } catch (err) {
            console.error('复制失败:', err);
            this.$emit('copy-error', '复制失败，请手动复制');
          } finally {
            document.body.removeChild(textArea);
          }
        }
      } catch (err) {
        console.error('复制到剪贴板失败:', err);
        this.$emit('copy-error', '复制失败，请检查浏览器权限');
      }
    },
    
    /**
     * 显示复制成功动画和消息
     */
    showCopySuccess(element, appName) {
      // 添加动画类
      element.classList.add('copy-success');
      
      // 发出成功事件
      this.$emit('copy-success', `📋 已复制 "${appName}" 的命令`);
      
      // 400ms后移除动画类
      setTimeout(() => {
        element.classList.remove('copy-success');
      }, 400);
    },
  }
}
</script>
