<template>
  <MSheet class="cover-sheet" :model-value="visible" :title="$t('apps.find_cover')" side="center" :close-label="$t('_common.close')" @update:model-value="(open) => !open && closeFinder()">
    <div class="cover-finder" :aria-busy="loading || applying">
      <form class="cover-finder__search" role="search" @submit.prevent="searchCovers">
        <MField ref="searchField" v-model="localSearchTerm" :placeholder="$t('apps.cover_search_placeholder')" :label="$t('apps.search_cover')" @keydown.enter="searchCovers" />
        <MIconButton v-if="localSearchTerm" icon="close" :label="$t('apps.clear_search')" @click="clearSearch" />
        <MButton icon="search" :loading="loading" :disabled="!localSearchTerm" @click="searchCovers">{{ $t('apps.search_cover') }}</MButton>
      </form>

      <MSeg v-model="coverFilter" :label="$t('apps.covers_found')" :options="tabs.map((tab) => ({ value: tab.key, label: `${$t(tab.labelKey)}${getTabCount(tab.key) ? ` ${getTabCount(tab.key)}` : ''}`, icon: tab.icon }))" />

      <div class="cover-finder__results">
        <div v-if="loading" class="cover-finder__state" role="status" aria-live="polite">
          <MLoading />
          <span>{{ $t('apps.cover_search_loading') }}</span>
        </div>
        <MEmpty v-else-if="failed" icon="cloud_off" :title="$t('apps.cover_search_failed')">
          <MButton variant="tonal" icon="refresh" @click="searchCovers">{{ $t('apps.search_cover') }}</MButton>
        </MEmpty>
        <MCoverGrid v-else-if="filteredCovers.length" :items="coverItems" :min="132" @select="onPick" @image-error="handleImageError">
          <template #badge="{ item }"><span class="m-cover__badges"><i class="apps-tag"><MIcon :name="item.source === 'steam' ? 'cloud' : 'sports_esports'" /></i></span></template>
        </MCoverGrid>
        <MEmpty v-else-if="searched" icon="search" :title="$t('apps.cover_search_empty')" />
        <MEmpty v-else icon="search" :title="$t('apps.cover_search_placeholder')" />
        <div v-if="applying" class="cover-finder__scrim" role="status" aria-live="polite"><MLoading /></div>
      </div>
    </div>
  </MSheet>
</template>

<script>
import { searchAllCovers } from '../utils/coverSearch.js'
import { apiPostJson } from '../utils/apiFetch.js'
import { MButton, MCoverGrid, MEmpty, MField, MIcon, MIconButton, MLoading, MSeg, MSheet } from 'foundation-ui/vue'

const createPlaceholderImage = (label) =>
  'data:image/svg+xml,' +
  encodeURIComponent(`
  <svg xmlns="http://www.w3.org/2000/svg" width="200" height="300" viewBox="0 0 200 300">
    <rect fill="#1a1a2e" width="200" height="300"/>
    <text x="100" y="150" text-anchor="middle" fill="#4a4a6a" font-size="14">${label}</text>
  </svg>
`)

const COVER_FINDER_TABS = [
  { key: 'all', icon: 'public', labelKey: 'apps.cover_search_tab_all' },
  { key: 'igdb', icon: 'sports_esports', labelKey: 'apps.cover_search_tab_igdb' },
  { key: 'steam', icon: 'cloud', labelKey: 'apps.cover_search_tab_steam' },
]

export default {
  name: 'CoverFinder',
  components: { MButton, MCoverGrid, MEmpty, MField, MIcon, MIconButton, MLoading, MSeg, MSheet },
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
      applying: false,
      searched: false,
      failed: false,
      igdbCovers: [],
      steamCovers: [],
      localSearchTerm: '',
      searchAbortController: null,
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
    coverItems() {
      return this.filteredCovers.map((cover, index) => ({ id: cover.key || `cover-${index}`, title: cover.name, cover: cover.url, source: cover.source, data: cover }))
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
    },
  },
  beforeUnmount() {
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
      this.searched = false
      this.failed = false
      this.$nextTick(() => this.$refs.searchField?.$el?.focus?.())
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
      this.searched = false
      this.failed = false
      this.abortPendingSearch()
      this.$refs.searchField?.$el?.focus?.()
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
      this.failed = false
      this.igdbCovers = []
      this.steamCovers = []

      try {
        const results = await searchAllCovers(this.localSearchTerm, currentController.signal)
        if (this.searchAbortController !== currentController) return
        this.igdbCovers = results.igdb
        this.steamCovers = results.steam
        this.searched = true
      } catch (error) {
        if (error.name === 'AbortError' || this.searchAbortController !== currentController) return
        console.error('Cover search failed:', error)
        this.failed = true
        this.$emit('error', this.$t('apps.cover_search_failed'))
      } finally {
        if (this.searchAbortController === currentController) {
          this.searchAbortController = null
          this.loading = false
        }
      }
    },

    onPick(item) {
      this.selectCover(item.data)
    },

    handleImageError(event) {
      const image = event.target
      if (image.dataset.coverFallbackApplied === 'true') return

      image.dataset.coverFallbackApplied = 'true'
      image.src = createPlaceholderImage(this.$t('apps.cover_image_unavailable'))
    },

    async selectCover(cover) {
      if (this.applying) return
      this.applying = true
      this.$emit('loading', true)

      try {
        const { path } = await apiPostJson('/api/covers/upload', { key: cover.key, url: cover.saveUrl })
        this.$emit('cover-selected', { path, source: cover.source })
        this.closeFinder()
      } catch (error) {
        console.error('Failed to apply cover:', error)
        this.$emit('error', this.$t('apps.cover_apply_failed'))
      } finally {
        this.applying = false
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
.cover-finder {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.cover-finder__search {
  display: flex;
  align-items: center;
  gap: 8px;
}

.cover-finder__search .m-field {
  flex: 1;
  min-width: 0;
}

.cover-finder__results {
  position: relative;
  min-height: 280px;
}

.cover-finder__state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
  min-height: 280px;
  color: var(--md-sys-color-on-surface-variant);
}

.cover-finder__scrim {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--md-sys-shape-corner-large);
  background: color-mix(in srgb, var(--md-sys-color-surface-container-high) 72%, transparent);
}
</style>
