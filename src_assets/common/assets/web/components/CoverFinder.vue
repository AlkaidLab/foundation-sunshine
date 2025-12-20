<template>
  <Transition name="finder-fade">
    <div v-if="visible" class="cover-finder-overlay" @click.self="closeFinder">
      <div class="cover-finder-panel" @click.stop>
        <!-- 头部 -->
        <div class="cover-finder__header">
          <div class="cover-finder__title">
            <i class="fas fa-image me-2"></i>
            <span>{{ $t('apps.find_cover') }}</span>
          </div>
          <button type="button" class="cover-finder__close" @click="closeFinder">
            <i class="fas fa-times"></i>
          </button>
        </div>

        <!-- 搜索框 -->
        <div class="cover-finder__search">
          <div class="cover-finder__search-wrapper">
            <i class="fas fa-search cover-finder__search-icon"></i>
            <input
              ref="searchInput"
              v-model="localSearchTerm"
              type="text"
              class="cover-finder__search-input"
              placeholder="输入游戏名称搜索..."
              @keydown.enter="searchCovers"
            />
            <button v-if="localSearchTerm" class="cover-finder__search-clear" @click="clearSearch" type="button">
              <i class="fas fa-times"></i>
            </button>
            <button class="cover-finder__search-btn" @click="searchCovers" :disabled="!localSearchTerm || loading" type="button">
              <i class="fas fa-arrow-right"></i>
            </button>
          </div>
        </div>

        <!-- 数据源筛选 -->
        <div class="cover-finder__tabs">
          <button
            v-for="tab in tabs"
            :key="tab.key"
            class="cover-finder__tab"
            :class="{ 'cover-finder__tab--active': coverFilter === tab.key }"
            @click.stop.prevent="coverFilter = tab.key"
          >
            <i :class="tab.icon"></i>
            <span>{{ tab.label }}</span>
            <span class="cover-finder__tab-badge" v-if="getTabCount(tab.key) > 0">
              {{ getTabCount(tab.key) }}
            </span>
          </button>
        </div>

        <!-- 内容区域 -->
        <div class="cover-finder__content">
          <!-- 加载状态 -->
          <div v-if="loading" class="cover-finder__loading">
            <div class="cover-finder__loading-spinner"></div>
            <p class="cover-finder__loading-text">正在搜索封面...</p>
          </div>

          <!-- 封面网格 -->
          <div v-else-if="filteredCovers.length > 0" class="cover-finder__grid">
            <div
              v-for="(cover, index) in filteredCovers"
              :key="cover.key || `cover-${index}`"
              class="cover-finder__card"
              @click="selectCover(cover)"
            >
              <div class="cover-finder__card-image">
                <img :src="cover.url" :alt="cover.name" loading="lazy" @error="handleImageError" />
                <div class="cover-finder__card-overlay">
                  <i class="fas fa-check-circle"></i>
                </div>
                <div class="cover-finder__card-badge" :class="`cover-finder__card-badge--${cover.source}`">
                  <i :class="cover.source === 'steam' ? 'fab fa-steam' : 'fas fa-gamepad'"></i>
                </div>
              </div>
              <div class="cover-finder__card-info">
                <p class="cover-finder__card-name" :title="cover.name">{{ cover.name }}</p>
              </div>
            </div>
          </div>

          <!-- 无结果 -->
          <div v-else class="cover-finder__empty">
            <div class="cover-finder__empty-icon">
              <i class="fas fa-search"></i>
            </div>
            <h4>未找到相关封面</h4>
            <p>尝试使用不同的关键词搜索</p>
          </div>
        </div>

        <!-- 底部提示 -->
        <div class="cover-finder__footer">
          <span class="cover-finder__footer-hint">
            <i class="fas fa-info-circle me-1"></i>
            点击封面即可应用
          </span>
        </div>
      </div>
    </div>
  </Transition>
</template>

<script>
import { searchAllCovers } from '../utils/coverSearch.js'

const PLACEHOLDER_IMAGE =
  'data:image/svg+xml,' +
  encodeURIComponent(`
  <svg xmlns="http://www.w3.org/2000/svg" width="200" height="300" viewBox="0 0 200 300">
    <rect fill="#1a1a2e" width="200" height="300"/>
    <text x="100" y="150" text-anchor="middle" fill="#4a4a6a" font-size="14">无法加载</text>
  </svg>
`)

export default {
  name: 'CoverFinder',
  props: {
    visible: {
      type: Boolean,
      default: false,
    },
    searchTerm: {
      type: String,
      default: '',
    },
  },
  emits: ['close', 'cover-selected', 'loading', 'error'],
  data() {
    return {
      coverFilter: 'all',
      loading: false,
      igdbCovers: [],
      steamCovers: [],
      localSearchTerm: '',
      searchAbortController: null,
      tabs: [
        { key: 'all', icon: 'fas fa-globe', label: '全部' },
        { key: 'igdb', icon: 'fas fa-gamepad', label: 'IGDB' },
        { key: 'steam', icon: 'fab fa-steam', label: 'Steam' },
      ],
    }
  },
  computed: {
    allCovers() {
      const result = []
      const maxLen = Math.max(this.igdbCovers.length, this.steamCovers.length)
      for (let i = 0; i < maxLen; i++) {
        if (i < this.igdbCovers.length) result.push(this.igdbCovers[i])
        if (i < this.steamCovers.length) result.push(this.steamCovers[i])
      }
      return result
    },
    filteredCovers() {
      const filterMap = {
        igdb: this.igdbCovers,
        steam: this.steamCovers,
        all: this.allCovers,
      }
      return filterMap[this.coverFilter] || this.allCovers
    },
  },
  watch: {
    visible(newVal) {
      if (newVal) {
        this.onOpen()
      } else {
        this.abortPendingSearch()
      }
      document.body.style.overflow = newVal ? 'hidden' : ''
    },
  },
  beforeUnmount() {
    document.body.style.overflow = ''
    this.abortPendingSearch()
  },
  methods: {
    getTabCount(key) {
      const countMap = {
        all: this.allCovers.length,
        igdb: this.igdbCovers.length,
        steam: this.steamCovers.length,
      }
      return countMap[key] || 0
    },

    onOpen() {
      this.localSearchTerm = this.searchTerm
      this.$nextTick(() => {
        this.$refs.searchInput?.focus()
        this.$refs.searchInput?.select()
      })
      if (this.localSearchTerm) {
        this.searchCovers()
      }
    },

    abortPendingSearch() {
      if (this.searchAbortController) {
        this.searchAbortController.abort()
        this.searchAbortController = null
      }
    },

    clearSearch() {
      this.localSearchTerm = ''
      this.igdbCovers = []
      this.steamCovers = []
      this.abortPendingSearch()
      this.$refs.searchInput?.focus()
    },

    async searchCovers() {
      if (!this.localSearchTerm) {
        this.igdbCovers = []
        this.steamCovers = []
        return
      }

      this.abortPendingSearch()
      this.searchAbortController = new AbortController()

      this.loading = true
      this.igdbCovers = []
      this.steamCovers = []

      try {
        const results = await searchAllCovers(this.localSearchTerm, this.searchAbortController.signal)
        this.igdbCovers = results.igdb
        this.steamCovers = results.steam
      } catch (error) {
        if (error.name === 'AbortError') return
        console.error('搜索封面失败:', error)
        this.$emit('error', '搜索封面失败，请稍后重试')
      } finally {
        this.loading = false
      }
    },

    handleImageError(event) {
      event.target.src = PLACEHOLDER_IMAGE
    },

    async selectCover(cover) {
      this.$emit('loading', true)

      try {
        if (cover.source === 'steam') {
          this.$emit('cover-selected', { path: cover.saveUrl, source: 'steam' })
        } else {
          const response = await fetch('/api/covers/upload', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ key: cover.key, url: cover.saveUrl }),
          })

          if (!response.ok) throw new Error('Failed to download cover')

          const { path } = await response.json()
          this.$emit('cover-selected', { path, source: 'igdb' })
        }
        this.closeFinder()
      } catch (error) {
        console.error('使用封面失败:', error)
        this.$emit('error', '使用封面失败，请稍后重试')
      } finally {
        this.$emit('loading', false)
      }
    },

    closeFinder() {
      this.$emit('close')
    },
  },
}
</script>

<style scoped lang="less">
@purple-dark: #7c3aed;
@purple-dark-deep: #5b21b6;
@purple-light: #6366f1;
@purple-light-deep: #4f46e5;

.cover-finder-overlay {
  position: fixed;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1050;
  padding: 2rem;
}

.cover-finder-panel {
  background: linear-gradient(145deg, #1e1e2e, #16161e);
  border-radius: 16px;
  width: 100%;
  max-width: 900px;
  max-height: 85vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5);
  overflow: hidden;

  [data-bs-theme='light'] & {
    background: linear-gradient(145deg, #f8faff, #f0f4ff);
    box-shadow: 0 25px 50px -12px rgba(99, 102, 241, 0.15);
  }
}

.cover-finder__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 1.25rem 1.5rem;
  border-bottom: 1px solid rgba(255, 255, 255, 0.06);

  [data-bs-theme='light'] & {
    border-bottom-color: rgba(99, 102, 241, 0.1);
  }
}

.cover-finder__title {
  display: flex;
  align-items: center;
  font-size: 1.1rem;
  font-weight: 600;
  color: #e0e0e0;

  i {
    color: @purple-dark;
  }

  [data-bs-theme='light'] & {
    color: #1e293b;

    i {
      color: @purple-light;
    }
  }
}

.cover-finder__close {
  width: 36px;
  height: 36px;
  border-radius: 10px;
  border: none;
  background: rgba(255, 255, 255, 0.05);
  color: #888;
  cursor: pointer;
  transition: all 0.2s;

  &:hover {
    background: rgba(239, 68, 68, 0.2);
    color: #ef4444;
  }

  [data-bs-theme='light'] & {
    background: rgba(99, 102, 241, 0.08);
    color: #64748b;

    &:hover {
      background: rgba(239, 68, 68, 0.15);
    }
  }
}

.cover-finder__search {
  padding: 1rem 1.5rem;

  &-wrapper {
    display: flex;
    align-items: center;
    background: rgba(0, 0, 0, 0.3);
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 12px;
    padding: 0 0.5rem;

    &:focus-within {
      border-color: rgba(124, 58, 237, 0.5);
    }

    [data-bs-theme='light'] & {
      background: #fff;
      border-color: rgba(99, 102, 241, 0.2);

      &:focus-within {
        border-color: rgba(99, 102, 241, 0.5);
        box-shadow: 0 0 0 3px rgba(99, 102, 241, 0.1);
      }
    }
  }

  &-icon {
    color: #666;
    padding: 0 0.75rem;

    [data-bs-theme='light'] & {
      color: #94a3b8;
    }
  }

  &-input {
    flex: 1;
    background: transparent;
    border: none;
    outline: none;
    color: #e0e0e0;
    padding: 0.85rem 0.25rem;

    &::placeholder {
      color: #555;
    }

    [data-bs-theme='light'] & {
      color: #1e293b;

      &::placeholder {
        color: #94a3b8;
      }
    }
  }

  &-clear,
  &-btn {
    width: 36px;
    height: 36px;
    border-radius: 8px;
    border: none;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.2s;
  }

  &-clear {
    background: transparent;
    color: #666;

    &:hover {
      color: #aaa;
    }

    [data-bs-theme='light'] & {
      color: #94a3b8;

      &:hover {
        color: #64748b;
      }
    }
  }

  &-btn {
    background: linear-gradient(135deg, @purple-dark, @purple-dark-deep);
    color: white;

    &:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }

    [data-bs-theme='light'] & {
      background: linear-gradient(135deg, @purple-light, @purple-light-deep);
    }
  }
}

.cover-finder__tabs {
  display: flex;
  gap: 0.5rem;
  padding: 0 1.5rem 1rem;
}

.cover-finder__tab {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.6rem 1.2rem;
  border-radius: 10px;
  border: none;
  background: rgba(255, 255, 255, 0.03);
  color: #888;
  cursor: pointer;
  transition: all 0.2s;

  &:hover {
    background: rgba(255, 255, 255, 0.06);
  }

  &--active {
    background: linear-gradient(135deg, @purple-dark, @purple-dark-deep);
    color: white;
  }

  [data-bs-theme='light'] & {
    background: rgba(99, 102, 241, 0.06);
    color: #64748b;

    &:hover {
      background: rgba(99, 102, 241, 0.1);
    }

    &--active {
      background: linear-gradient(135deg, @purple-light, @purple-light-deep);
      color: white;
    }
  }

  &-badge {
    background: rgba(255, 255, 255, 0.2);
    padding: 0.1rem 0.5rem;
    border-radius: 10px;
    font-size: 0.75rem;
  }
}

.cover-finder__content {
  flex: 1;
  overflow-y: auto;
  padding: 1.5rem;
  min-height: 300px;

  &::-webkit-scrollbar {
    width: 8px;
  }

  &::-webkit-scrollbar-thumb {
    background: rgba(255, 255, 255, 0.1);
    border-radius: 4px;
  }

  [data-bs-theme='light'] & {
    &::-webkit-scrollbar-thumb {
      background: rgba(99, 102, 241, 0.2);
    }
  }
}

.cover-finder__loading {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  padding: 4rem 2rem;

  &-spinner {
    width: 40px;
    height: 40px;
    border: 3px solid rgba(124, 58, 237, 0.2);
    border-top-color: @purple-dark;
    border-radius: 50%;
    animation: cover-finder-spin 1s linear infinite;

    [data-bs-theme='light'] & {
      border-color: rgba(99, 102, 241, 0.2);
      border-top-color: @purple-light;
    }
  }

  &-text {
    margin-top: 1rem;
    color: #888;

    [data-bs-theme='light'] & {
      color: #64748b;
    }
  }
}

@keyframes cover-finder-spin {
  to {
    transform: rotate(360deg);
  }
}

.cover-finder__grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
  gap: 1.25rem;
}

.cover-finder__card {
  cursor: pointer;
  transition: transform 0.2s;

  &:hover {
    transform: translateY(-4px);

    .cover-finder__card-overlay {
      opacity: 1;
    }
  }

  &-image {
    position: relative;
    aspect-ratio: 2/3;
    border-radius: 10px;
    overflow: hidden;
    background: #0d0d14;

    img {
      width: 100%;
      height: 100%;
      object-fit: cover;
    }

    [data-bs-theme='light'] & {
      background: #e8efff;
    }
  }

  &-overlay {
    position: absolute;
    inset: 0;
    background: rgba(124, 58, 237, 0.85);
    display: flex;
    align-items: center;
    justify-content: center;
    opacity: 0;
    transition: opacity 0.2s;
    color: white;
    font-size: 2rem;

    [data-bs-theme='light'] & {
      background: rgba(99, 102, 241, 0.85);
    }
  }

  &-badge {
    position: absolute;
    top: 8px;
    right: 8px;
    width: 24px;
    height: 24px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 0.7rem;
    color: white;

    &--steam {
      background: #1b2838;
    }

    &--igdb {
      background: #9147ff;
    }
  }

  &-info {
    padding: 0.5rem 0;
  }

  &-name {
    font-size: 0.8rem;
    color: #d0d0d0;
    margin: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;

    [data-bs-theme='light'] & {
      color: #475569;
    }
  }
}

.cover-finder__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 3rem 2rem;
  text-align: center;

  h4 {
    color: #aaa;
    margin: 0 0 0.5rem;
  }

  p {
    color: #666;
    margin: 0;
  }

  [data-bs-theme='light'] & {
    h4 {
      color: #475569;
    }

    p {
      color: #94a3b8;
    }
  }

  &-icon {
    width: 80px;
    height: 80px;
    border-radius: 50%;
    background: rgba(255, 255, 255, 0.03);
    display: flex;
    align-items: center;
    justify-content: center;
    margin-bottom: 1.5rem;

    i {
      font-size: 2rem;
      color: #4a4a6a;
    }

    [data-bs-theme='light'] & {
      background: rgba(99, 102, 241, 0.08);

      i {
        color: #94a3b8;
      }
    }
  }
}

.cover-finder__footer {
  padding: 0.75rem 1.5rem;
  border-top: 1px solid rgba(255, 255, 255, 0.06);

  [data-bs-theme='light'] & {
    border-top-color: rgba(99, 102, 241, 0.1);
  }

  &-hint {
    font-size: 0.8rem;
    color: #666;

    i {
      color: @purple-dark;
    }

    [data-bs-theme='light'] & {
      color: #64748b;

      i {
        color: @purple-light;
      }
    }
  }
}

.finder-fade-enter-active,
.finder-fade-leave-active {
  transition: opacity 0.3s;
}

.finder-fade-enter-from,
.finder-fade-leave-to {
  opacity: 0;
}

@media (max-width: 768px) {
  .cover-finder-overlay {
    padding: 1rem;
  }

  .cover-finder__grid {
    grid-template-columns: repeat(auto-fill, minmax(100px, 1fr));
    gap: 1rem;
  }
}
</style>
