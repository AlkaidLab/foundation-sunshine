<template>
  <Teleport to="body">
    <Transition name="finder-fade">
      <div v-if="visible" class="cover-finder-overlay" @click.self="closeFinder">
        <div
          ref="panel"
          class="cover-finder-panel"
          role="dialog"
          aria-modal="true"
          aria-labelledby="coverFinderTitle"
          tabindex="-1"
          @click.stop
          @keydown.esc="closeFinder"
          @keydown.tab="handleTabKeydown"
        >
          <!-- 头部 -->
          <div class="cover-finder__header">
            <div id="coverFinderTitle" class="cover-finder__title">
              <i class="fas fa-image me-2"></i>
              <span>{{ $t('apps.find_cover') }}</span>
            </div>
            <button
              type="button"
              class="cover-finder__close"
              :aria-label="$t('_common.close')"
              @click="closeFinder"
            >
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
                :placeholder="$t('apps.cover_search_placeholder')"
                @keydown.enter="searchCovers"
              />
              <button
                v-if="localSearchTerm"
                class="cover-finder__search-clear"
                type="button"
                :aria-label="$t('apps.clear_search')"
                @click="clearSearch"
              >
                <i class="fas fa-times"></i>
              </button>
              <button
                class="cover-finder__search-btn"
                type="button"
                :aria-label="$t('apps.search_cover')"
                :disabled="!localSearchTerm || loading"
                @click="searchCovers"
              >
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
              type="button"
              :class="{ 'cover-finder__tab--active': coverFilter === tab.key }"
              :aria-pressed="coverFilter === tab.key"
              @click.stop.prevent="coverFilter = tab.key"
            >
              <i :class="tab.icon"></i>
              <span>{{ $t(tab.labelKey) }}</span>
              <span class="cover-finder__tab-badge" v-if="getTabCount(tab.key) > 0">
                {{ getTabCount(tab.key) }}
              </span>
            </button>
          </div>

          <!-- 内容区域 -->
          <div class="cover-finder__content">
            <!-- 加载状态 -->
            <div v-if="loading" class="cover-finder__loading" role="status" aria-live="polite">
              <div class="cover-finder__loading-spinner"></div>
              <p class="cover-finder__loading-text">{{ $t('apps.cover_search_loading') }}</p>
            </div>

            <!-- 封面网格 -->
            <div v-else-if="filteredCovers.length > 0" class="cover-finder__grid">
              <button
                v-for="(cover, index) in filteredCovers"
                :key="cover.key || `cover-${index}`"
                type="button"
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
              </button>
            </div>

            <!-- 无结果 -->
            <div v-else class="cover-finder__empty">
              <div class="cover-finder__empty-icon">
                <i class="fas fa-search"></i>
              </div>
              <h4>{{ $t('apps.cover_search_empty') }}</h4>
              <p>{{ $t('apps.cover_search_empty_desc') }}</p>
            </div>
          </div>

          <!-- 底部提示 -->
          <div class="cover-finder__footer">
            <span class="cover-finder__footer-hint">
              <i class="fas fa-info-circle me-1"></i>
              {{ $t('apps.cover_search_hint') }}
            </span>
          </div>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>

<script>
import { searchAllCovers } from '../utils/coverSearch.js'
import { apiPostJson } from '../utils/apiFetch.js'
import { getFocusableElements } from '../utils/focus.js'

const createPlaceholderImage = (label) =>
  'data:image/svg+xml,' +
  encodeURIComponent(`
  <svg xmlns="http://www.w3.org/2000/svg" width="200" height="300" viewBox="0 0 200 300">
    <rect fill="#1a1a2e" width="200" height="300"/>
    <text x="100" y="150" text-anchor="middle" fill="#4a4a6a" font-size="14">${label}</text>
  </svg>
`)

const COVER_FINDER_TABS = [
  { key: 'all', icon: 'fas fa-globe', labelKey: 'apps.cover_search_tab_all' },
  { key: 'igdb', icon: 'fas fa-gamepad', labelKey: 'apps.cover_search_tab_igdb' },
  { key: 'steam', icon: 'fab fa-steam', labelKey: 'apps.cover_search_tab_steam' },
]

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
      previousFocus: null,
    }
  },
  computed: {
    tabs() {
      return COVER_FINDER_TABS
    },
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
        this.$nextTick(() => this.restoreFocus())
      }
      document.body.style.overflow = newVal ? 'hidden' : ''
    },
  },
  beforeUnmount() {
    document.body.style.overflow = ''
    this.abortPendingSearch()
    this.restoreFocus()
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
      this.previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
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
      const controller = this.searchAbortController
      this.searchAbortController = null
      controller?.abort()
      this.loading = false
    },

    clearSearch() {
      this.localSearchTerm = ''
      this.igdbCovers = []
      this.steamCovers = []
      this.abortPendingSearch()
      this.$refs.searchInput?.focus()
    },

    handleTabKeydown(event) {
      const focusable = getFocusableElements(this.$refs.panel)
      if (!focusable.length) {
        event.preventDefault()
        this.$refs.panel?.focus()
        return
      }

      const first = focusable[0]
      const last = focusable[focusable.length - 1]
      const panel = this.$refs.panel
      if (event.shiftKey && (document.activeElement === first || !panel?.contains(document.activeElement))) {
        event.preventDefault()
        last.focus()
      } else if (!event.shiftKey && (document.activeElement === last || !panel?.contains(document.activeElement))) {
        event.preventDefault()
        first.focus()
      }
    },

    restoreFocus() {
      if (this.previousFocus instanceof HTMLElement) {
        this.previousFocus.focus()
        this.previousFocus = null
      }
    },

    async searchCovers() {
      if (!this.localSearchTerm) {
        this.abortPendingSearch()
        this.igdbCovers = []
        this.steamCovers = []
        return
      }

      this.abortPendingSearch()
      const currentController = new AbortController()
      this.searchAbortController = currentController

      this.loading = true
      this.igdbCovers = []
      this.steamCovers = []

      try {
        const results = await searchAllCovers(this.localSearchTerm, currentController.signal)
        if (this.searchAbortController !== currentController) return
        this.igdbCovers = results.igdb
        this.steamCovers = results.steam
      } catch (error) {
        if (error.name === 'AbortError' || this.searchAbortController !== currentController) return
        console.error('Cover search failed:', error)
        this.$emit('error', this.$t('apps.cover_search_failed'))
      } finally {
        if (this.searchAbortController === currentController) {
          this.searchAbortController = null
          this.loading = false
        }
      }
    },

    handleImageError(event) {
      const image = event.target
      if (image.dataset.coverFallbackApplied === 'true') return

      image.dataset.coverFallbackApplied = 'true'
      image.src = createPlaceholderImage(this.$t('apps.cover_image_unavailable'))
    },

    async selectCover(cover) {
      this.$emit('loading', true)

      try {
        const { path } = await apiPostJson('/api/covers/upload', { key: cover.key, url: cover.saveUrl })
        this.$emit('cover-selected', { path, source: cover.source })
        this.closeFinder()
      } catch (error) {
        console.error('Failed to apply cover:', error)
        this.$emit('error', this.$t('apps.cover_apply_failed'))
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
.cover-finder-overlay {
  position: fixed;
  inset: 0;
  z-index: 1070;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 2rem;
  background: var(--ui-overlay);
  backdrop-filter: blur(6px);
}

.cover-finder-panel {
  display: flex;
  flex-direction: column;
  width: min(900px, 100%);
  max-height: min(85vh, 760px);
  overflow: hidden;
  color: var(--ui-text-primary);
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-lg);
  box-shadow: var(--ui-shadow-lg);
}

.cover-finder__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 1rem;
  padding: 1.1rem 1.25rem;
  border-bottom: 1px solid var(--ui-border);
}

.cover-finder__title {
  display: flex;
  align-items: center;
  min-width: 0;
  color: var(--ui-text-primary);
  font-size: 1.05rem;
  font-weight: 650;

  i {
    color: var(--ui-accent);
  }
}

.cover-finder__close,
.cover-finder__search-clear,
.cover-finder__search-btn {
  display: inline-flex;
  flex: 0 0 auto;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  padding: 0;
  border-radius: var(--ui-radius-sm);
  cursor: pointer;
  transition:
    background-color 0.18s ease,
    border-color 0.18s ease,
    color 0.18s ease,
    transform 0.18s ease;

  &:focus-visible {
    outline: 3px solid var(--ui-accent-soft);
    outline-offset: 2px;
  }
}

.cover-finder__close {
  color: var(--ui-text-secondary);
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);

  &:hover {
    color: var(--ui-danger-text);
    background: var(--ui-danger-soft);
    border-color: var(--ui-danger-border);
  }
}

.cover-finder__search {
  padding: 1rem 1.25rem 0.85rem;

  &-wrapper {
    display: flex;
    align-items: center;
    min-width: 0;
    padding: 0 0.45rem;
    background: var(--ui-surface);
    border: 1px solid var(--ui-border);
    border-radius: var(--ui-radius-md);
    transition:
      border-color 0.18s ease,
      box-shadow 0.18s ease,
      background-color 0.18s ease;

    &:focus-within {
      background: var(--ui-surface-hover);
      border-color: var(--ui-accent);
      box-shadow: 0 0 0 3px var(--ui-accent-soft);
    }
  }

  &-icon {
    padding: 0 0.7rem;
    color: var(--ui-text-muted);
  }

  &-input {
    flex: 1;
    min-width: 0;
    padding: 0.8rem 0.25rem;
    color: var(--ui-text-primary);
    font: inherit;
    background: transparent;
    border: 0;
    outline: 0;

    &::placeholder {
      color: var(--ui-text-muted);
    }
  }

  &-clear {
    color: var(--ui-text-muted);
    background: transparent;
    border: 0;

    &:hover {
      color: var(--ui-text-primary);
      background: var(--ui-surface-hover);
    }
  }

  &-btn {
    color: var(--ui-accent-contrast);
    background: var(--ui-accent);
    border: 1px solid var(--ui-accent);

    &:hover:not(:disabled) {
      transform: translateY(-1px);
    }

    &:active:not(:disabled) {
      transform: scale(0.96);
    }

    &:disabled {
      color: var(--ui-text-muted);
      cursor: not-allowed;
      background: var(--ui-surface);
      border-color: var(--ui-border);
    }
  }
}

.cover-finder__tabs {
  display: flex;
  gap: 0.5rem;
  padding: 0 1.25rem 1rem;
  overflow-x: auto;
  scrollbar-width: none;

  &::-webkit-scrollbar {
    display: none;
  }
}

.cover-finder__tab {
  display: inline-flex;
  flex: 0 0 auto;
  align-items: center;
  gap: 0.45rem;
  min-height: 38px;
  padding: 0.5rem 0.9rem;
  color: var(--ui-text-secondary);
  white-space: nowrap;
  cursor: pointer;
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-sm);
  transition:
    background-color 0.18s ease,
    border-color 0.18s ease,
    color 0.18s ease;

  &:hover {
    color: var(--ui-text-primary);
    background: var(--ui-surface-hover);
    border-color: var(--ui-border-strong);
  }

  &:focus-visible {
    outline: 3px solid var(--ui-accent-soft);
    outline-offset: 2px;
  }

  &--active {
    color: var(--ui-accent-contrast);
    background: var(--ui-accent);
    border-color: var(--ui-accent);
  }

  &-badge {
    min-width: 1.25rem;
    padding: 0.08rem 0.4rem;
    color: inherit;
    font-size: 0.72rem;
    text-align: center;
    background: var(--ui-accent-soft);
    border-radius: 999px;
  }
}

.cover-finder__content {
  flex: 1;
  min-height: 300px;
  padding: 1.25rem;
  overflow-y: auto;
  border-top: 1px solid var(--ui-border);

  &::-webkit-scrollbar {
    width: 8px;
  }

  &::-webkit-scrollbar-thumb {
    background: var(--ui-border-strong);
    border-radius: 999px;
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
    border: 3px solid var(--ui-accent-soft);
    border-top-color: var(--ui-accent);
    border-radius: 50%;
    animation: cover-finder-spin 1s linear infinite;
  }

  &-text {
    margin: 1rem 0 0;
    color: var(--ui-text-secondary);
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
  gap: 1rem;
}

.cover-finder__card {
  min-width: 0;
  padding: 0;
  overflow: hidden;
  color: var(--ui-text-primary);
  text-align: left;
  cursor: pointer;
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-md);
  transition:
    transform 0.18s ease,
    border-color 0.18s ease,
    box-shadow 0.18s ease;

  &:hover {
    border-color: var(--ui-border-strong);
    box-shadow: var(--ui-shadow-sm);
    transform: translateY(-2px);

    .cover-finder__card-overlay {
      opacity: 1;
    }
  }

  &:focus-visible {
    outline: 3px solid var(--ui-accent-soft);
    outline-offset: 2px;
  }

  &-image {
    position: relative;
    aspect-ratio: 2 / 3;
    overflow: hidden;
    background: var(--ui-surface-hover);
    border-bottom: 1px solid var(--ui-border);

    img {
      width: 100%;
      height: 100%;
      object-fit: cover;
    }
  }

  &-overlay {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    color: var(--ui-accent-contrast);
    font-size: 2rem;
    background: rgba(var(--ui-accent-rgb), 0.82);
    opacity: 0;
    transition: opacity 0.18s ease;
  }

  &-badge {
    position: absolute;
    top: 8px;
    right: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 26px;
    color: #fff;
    font-size: 0.72rem;
    border: 1px solid rgba(255, 255, 255, 0.24);
    border-radius: 7px;
    box-shadow: var(--ui-shadow-sm);

    &--steam {
      background: #1b2838;
    }

    &--igdb {
      background: #9147ff;
    }
  }

  &-info {
    padding: 0.65rem 0.75rem;
  }

  &-name {
    margin: 0;
    overflow: hidden;
    color: var(--ui-text-primary);
    font-size: 0.82rem;
    font-weight: 600;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
}

.cover-finder__empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 3rem 2rem;
  text-align: center;

  h4 {
    margin: 0 0 0.5rem;
    color: var(--ui-text-primary);
  }

  p {
    margin: 0;
    color: var(--ui-text-secondary);
  }

  &-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 72px;
    height: 72px;
    margin-bottom: 1.25rem;
    color: var(--ui-accent);
    background: var(--ui-accent-soft);
    border: 1px solid var(--ui-border);
    border-radius: 50%;

    i {
      font-size: 1.8rem;
    }
  }
}

.cover-finder__footer {
  padding: 0.75rem 1.25rem;
  background: var(--ui-surface);
  border-top: 1px solid var(--ui-border);

  &-hint {
    color: var(--ui-text-secondary);
    font-size: 0.8rem;

    i {
      color: var(--ui-accent);
    }
  }
}

.finder-fade-enter-active,
.finder-fade-leave-active {
  transition: opacity 0.2s ease;
}

.finder-fade-enter-from,
.finder-fade-leave-to {
  opacity: 0;
}

@media (max-width: 768px) {
  .cover-finder-overlay {
    align-items: stretch;
    padding: 0.5rem;
  }

  .cover-finder-panel {
    max-height: calc(100dvh - 1rem);
  }

  .cover-finder__header {
    padding: 0.9rem 1rem;
  }

  .cover-finder__search {
    padding: 0.8rem 1rem 0.7rem;
  }

  .cover-finder__tabs {
    padding: 0 1rem 0.8rem;
  }

  .cover-finder__content {
    min-height: 0;
    padding: 1rem;
  }

  .cover-finder__grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 0.75rem;
  }

  .cover-finder__empty {
    padding: 2.5rem 1rem;
  }

  .cover-finder__footer {
    padding: 0.7rem 1rem;
  }
}

@media (prefers-reduced-motion: reduce) {
  .cover-finder__card,
  .cover-finder__card-overlay,
  .cover-finder__search-btn,
  .cover-finder__loading-spinner,
  .finder-fade-enter-active,
  .finder-fade-leave-active {
    transition: none;
  }

  .cover-finder__loading-spinner {
    animation: none;
  }
}
</style>
