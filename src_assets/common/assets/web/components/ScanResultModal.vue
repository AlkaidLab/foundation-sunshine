<template>
  <Transition name="fade">
    <div v-if="show" class="scan-result-overlay" @click.self="$emit('close')">
      <div class="scan-result-modal">
        <!-- 标题栏 -->
        <div class="scan-result-header">
          <h5>
            <i class="fas fa-search me-2"></i>{{ t('apps.scan_result_title') }}
            <span class="badge bg-primary ms-2">{{ apps.length }}</span>
            <span v-if="stats.games > 0" class="badge bg-warning text-dark ms-2">
              <i class="fas fa-gamepad me-1"></i>{{ stats.games }}
            </span>
            <span v-if="hasActiveFilter" class="badge bg-info ms-2"> {{ t('apps.scan_result_matched', { count: filteredAppItems.length }) }} </span>
          </h5>
          <button class="btn-close" :aria-label="t('close')" @click="$emit('close')"></button>
        </div>

        <!-- 搜索框和过滤器 -->
        <div v-if="apps.length > 0" class="scan-result-search">
          <div class="search-box">
            <i class="fas fa-search search-icon"></i>
            <input
              type="text"
              class="form-control search-input"
              :placeholder="t('apps.scan_result_search_placeholder')"
              v-model="searchQuery"
            />
            <button v-if="searchQuery" class="btn-clear-search" @click="searchQuery = ''" type="button">
              <i class="fas fa-times"></i>
            </button>
          </div>

          <!-- 过滤器按钮组 -->
          <div class="scan-result-filters mt-2">
            <div class="d-flex flex-wrap gap-2 align-items-center">
              <!-- 应用类型过滤 -->
              <div class="btn-group btn-group-sm flex-wrap" role="group">
                <button
                  class="btn"
                  :class="selectedType === 'all' ? 'btn-primary' : 'btn-outline-primary'"
                  @click="selectedType = 'all'"
                  type="button"
                >
                  {{ t('apps.scan_result_filter_all') }}
                  <span class="badge bg-dark ms-1">{{ stats.all }}</span>
                </button>
                <button
                  v-if="stats.shortcut > 0"
                  class="btn"
                  :class="selectedType === 'shortcut' ? 'btn-info' : 'btn-outline-info'"
                  @click="selectedType = 'shortcut'"
                  type="button"
                  :title="t('apps.scan_result_filter_shortcut_title')"
                >
                  <i class="fas fa-link me-1"></i>{{ t('apps.scan_result_filter_shortcut') }}
                  <span class="badge bg-dark ms-1">{{ stats.shortcut }}</span>
                </button>
                <button
                  v-if="stats.executable > 0"
                  class="btn"
                  :class="selectedType === 'executable' ? 'btn-primary' : 'btn-outline-primary'"
                  @click="selectedType = 'executable'"
                  type="button"
                  :title="t('apps.scan_result_filter_executable_title')"
                >
                  <i class="fas fa-file-code me-1"></i>{{ t('apps.scan_result_filter_executable') }}
                  <span class="badge bg-dark ms-1">{{ stats.executable }}</span>
                </button>
                <button
                  v-if="stats.batch > 0 || stats.command > 0"
                  class="btn"
                  :class="
                    selectedType === 'batch' || selectedType === 'command' ? 'btn-secondary' : 'btn-outline-secondary'
                  "
                  @click="selectedType = stats.batch > 0 ? 'batch' : 'command'"
                  type="button"
                  :title="t('apps.scan_result_filter_script_title')"
                >
                  <i class="fas fa-terminal me-1"></i>{{ t('apps.scan_result_filter_script') }}
                  <span class="badge bg-dark ms-1">{{ stats.batch + stats.command }}</span>
                </button>
                <button
                  v-if="stats.url > 0"
                  class="btn"
                  :class="selectedType === 'url' ? 'btn-success' : 'btn-outline-success'"
                  @click="selectedType = 'url'"
                  type="button"
                  :title="t('apps.scan_result_filter_url_title')"
                >
                  <i class="fas fa-globe me-1"></i>{{ t('apps.scan_result_filter_url') }}
                  <span class="badge bg-dark ms-1">{{ stats.url }}</span>
                </button>
                <button
                  v-if="stats.steam > 0"
                  class="btn"
                  :class="selectedType === 'steam' ? 'btn-primary' : 'btn-outline-primary'"
                  @click="selectedType = 'steam'"
                  type="button"
                  :title="t('apps.scan_result_filter_steam_title')"
                >
                  <i class="fab fa-steam me-1"></i>Steam
                  <span class="badge bg-dark ms-1">{{ stats.steam }}</span>
                </button>
                <button
                  v-if="stats.epic > 0"
                  class="btn"
                  :class="selectedType === 'epic' ? 'btn-dark' : 'btn-outline-dark'"
                  @click="selectedType = 'epic'"
                  type="button"
                  :title="t('apps.scan_result_filter_epic_title')"
                >
                  <i class="fas fa-store me-1"></i>Epic
                  <span class="badge bg-dark ms-1">{{ stats.epic }}</span>
                </button>
                <button
                  v-if="stats.gog > 0"
                  class="btn"
                  :class="selectedType === 'gog' ? 'btn-secondary' : 'btn-outline-secondary'"
                  @click="selectedType = 'gog'"
                  type="button"
                  :title="t('apps.scan_result_filter_gog_title')"
                >
                  <i class="fas fa-compact-disc me-1"></i>GOG
                  <span class="badge bg-dark ms-1">{{ stats.gog }}</span>
                </button>
              </div>

              <!-- 游戏过滤 -->
              <button
                v-if="stats.games > 0"
                class="btn btn-sm"
                :class="gamesOnly ? 'btn-warning' : 'btn-outline-warning'"
                @click="gamesOnly = !gamesOnly"
                type="button"
              >
                <i class="fas fa-gamepad me-1"></i>
                {{ gamesOnly ? t('apps.scan_result_show_all') : t('apps.scan_result_games_only') }}
                <span class="badge bg-dark ms-1">{{ stats.games }}</span>
              </button>
              <button
                v-if="stats.review > 0"
                class="btn btn-sm"
                :class="reviewOnly ? 'btn-danger' : 'btn-outline-danger'"
                :aria-pressed="reviewOnly ? 'true' : 'false'"
                @click="reviewOnly = !reviewOnly"
                type="button"
              >
                <i class="fas fa-triangle-exclamation me-1"></i>
                {{ reviewLabel }}
                <span class="badge bg-dark ms-1">{{ stats.review }}</span>
              </button>
            </div>
          </div>
        </div>

        <div v-if="enhancementProgress.active" class="scan-result-progress" role="status" aria-live="polite">
          <div class="scan-progress-copy">
            <i class="fas fa-wand-magic-sparkles"></i>
            <span>{{ enhancementProgress.stage }}</span>
            <small v-if="enhancementProgress.detail">{{ enhancementProgress.detail }}</small>
          </div>
          <div v-if="enhancementProgress.total" class="scan-progress-count">
            {{ enhancementProgress.current }}/{{ enhancementProgress.total }}
          </div>
          <div
            class="scan-progress-bar"
            :class="{ 'scan-progress-bar--indeterminate': enhancementProgress.indeterminate }"
          >
            <span :style="{ width: enhancementProgress.indeterminate ? '42%' : `${enhancementProgressPercent}%` }"></span>
          </div>
        </div>

        <!-- 应用列表 -->
        <div class="scan-result-body">
          <div v-if="apps.length === 0" class="text-center text-muted py-4">
            <i class="fas fa-folder-open fa-3x mb-3"></i>
            <p>{{ t('apps.scan_result_no_apps') }}</p>
          </div>
          <div v-else-if="filteredAppItems.length === 0" class="text-center text-muted py-4">
            <i class="fas fa-search fa-3x mb-3"></i>
            <p>{{ t('apps.scan_result_no_matches') }}</p>
            <p class="small">{{ t('apps.scan_result_try_different_keywords') }}</p>
          </div>
          <div v-else class="scan-result-list">
            <div
              v-for="{ app, index, review, reviewReasons } in filteredAppItems"
              :key="`${app.source_path || app.cmd || app.name}-${index}`"
              class="scan-result-item"
              :class="{ 'scan-result-item--review': review }"
            >
              <!-- 应用图标 -->
              <div class="scan-app-icon">
                <img
                  v-if="app['image-path']"
                  :src="app['image-path']"
                  :alt="app.name"
                  loading="lazy"
                  decoding="async"
                  @error="$event.target.style.display = 'none'"
                />
                <svg v-else width="80" height="80" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
                  <rect width="100" height="100" fill="#667eea" />
                  <text
                    x="50"
                    y="50"
                    font-size="40"
                    font-weight="bold"
                    fill="#fff"
                    text-anchor="middle"
                    dominant-baseline="central"
                  >
                    {{ app.name.charAt(0).toUpperCase() }}
                  </text>
                </svg>
              </div>

              <!-- 应用信息 -->
              <div class="scan-app-info">
                <div class="scan-app-name">
                  <i v-if="app['is-game']" class="fas fa-gamepad me-1 text-warning" :title="t('apps.scan_result_game')"></i>
                  {{ app.name }}
                  <span v-if="app['app-type']" class="badge ms-2" :class="getAppTypeBadgeClass(app['app-type'])">
                    {{ getAppTypeLabel(app['app-type']) }}
                  </span>
                  <span v-if="app['is-game']" class="badge bg-warning text-dark ms-2">
                    <i class="fas fa-gamepad me-1"></i>{{ t('apps.scan_result_game') }}
                  </span>
                  <span v-if="review" class="badge bg-danger ms-2">
                    <i class="fas fa-triangle-exclamation me-1"></i>{{ reviewLabel }}
                  </span>
                </div>
                <div v-if="app['original-name'] && app['original-name'] !== app.name" class="scan-app-cmd small">
                  Original: {{ app['original-name'] }}
                </div>
                <div v-if="app['canonical-name'] || hasNumericValue(app['ai-confidence'])" class="scan-app-cmd small">
                  <i class="fas fa-wand-magic-sparkles me-1"></i>
                  {{ app['canonical-name'] || app.name }}
                  <span v-if="hasNumericValue(app['ai-confidence'])" class="badge bg-info ms-1">
                    {{ Math.round(app['ai-confidence'] * 100) }}%
                  </span>
                </div>
                <div v-if="app['cover-source'] || app['cover-match-name']" class="scan-app-cmd small">
                  <i class="fas fa-image me-1"></i>
                  {{ app['cover-source'] || 'cover' }}
                  <span v-if="app['cover-match-name']">: {{ app['cover-match-name'] }}</span>
                  <span v-if="hasNumericValue(app['ai-cover-confidence'])" class="badge bg-info ms-1">
                    {{ Math.round(app['ai-cover-confidence'] * 100) }}%
                  </span>
                </div>
                <div v-if="review" class="scan-app-review small">
                  <i class="fas fa-triangle-exclamation me-1"></i>
                  {{ reviewReasons.join(' / ') }}
                </div>
                <div class="scan-app-cmd small">{{ app.cmd }}</div>
                <div class="scan-app-path small"><i class="fas fa-folder-open me-1"></i>{{ app.source_path }}</div>
              </div>

              <!-- 操作按钮 -->
              <div class="scan-app-actions">
                <button class="btn btn-sm btn-outline-primary" @click="$emit('edit', app)" :title="t('apps.scan_result_edit_title')">
                  <i class="fas fa-edit"></i>
                </button>
                <button
                  class="btn btn-sm btn-outline-success"
                  @click="$emit('quick-add', app, index)"
                  :title="t('apps.scan_result_quick_add_title')"
                >
                  <i class="fas fa-plus"></i>
                </button>
                <button
                  class="btn btn-sm btn-outline-danger"
                  @click="$emit('remove', index)"
                  :title="t('apps.scan_result_remove_title')"
                >
                  <i class="fas fa-times"></i>
                </button>
              </div>
            </div>
          </div>
        </div>

        <!-- 底部操作栏 -->
        <div v-if="apps.length > 0" class="scan-result-footer">
          <button class="btn btn-secondary" @click="$emit('close')"><i class="fas fa-times me-1"></i>{{ t('_common.cancel') }}</button>
          <button class="btn btn-primary" @click="$emit('add-all')" :disabled="saving">
            <i class="fas" :class="saving ? 'fa-spinner fa-spin' : 'fa-check-double'"></i>
            <span class="ms-1">{{ t('apps.scan_result_add_all') }}</span>
          </button>
        </div>
      </div>
    </div>
  </Transition>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import {
  getGameResourceReviewReasons,
  needsGameResourceReview,
} from '../utils/agents/gameLibrary/gameLibraryCuratorAgent.js'

const { t, locale } = useI18n()

const props = defineProps({
  show: {
    type: Boolean,
    default: false,
  },
  apps: {
    type: Array,
    default: () => [],
  },
  saving: {
    type: Boolean,
    default: false,
  },
  enhancementProgress: {
    type: Object,
    default: () => ({
      active: false,
      stage: '',
      detail: '',
      current: 0,
      total: 0,
      indeterminate: false,
    }),
  },
  enhancementProgressPercent: {
    type: Number,
    default: 0,
  },
})

defineEmits(['close', 'edit', 'quick-add', 'remove', 'add-all'])

// 本地状态
const searchQuery = ref('')
const selectedType = ref('all')
const gamesOnly = ref(false)
const reviewOnly = ref(false)

const isChineseLocale = computed(() => String(locale.value || '').toLowerCase().startsWith('zh'))
const reviewLabel = computed(() => (isChineseLocale.value ? '需要审核' : 'Review'))

// 重置过滤器
watch(
  () => props.show,
  (newVal) => {
    if (!newVal) {
      searchQuery.value = ''
      selectedType.value = 'all'
      gamesOnly.value = false
      reviewOnly.value = false
    }
  }
)

// 当 apps 引用被整体替换时（新扫描结果），重置所有筛选状态
watch(
  () => props.apps,
  (newApps) => {
    const hasGames = newApps.length > 0 && newApps.some((app) => app['is-game'] === true)
    gamesOnly.value = hasGames
    selectedType.value = 'all'
    searchQuery.value = ''
    reviewOnly.value = false
  }
)

// 统计信息
const stats = computed(() => {
  const totals = {
    all: props.apps.length,
    games: 0,
    executable: 0,
    shortcut: 0,
    batch: 0,
    command: 0,
    url: 0,
    steam: 0,
    epic: 0,
    gog: 0,
    review: 0,
  }

  for (const app of props.apps) {
    if (app['is-game'] === true) {
      totals.games += 1
    }
    if (Object.prototype.hasOwnProperty.call(totals, app['app-type'])) {
      totals[app['app-type']] += 1
    }
    if (needsReview(app)) {
      totals.review += 1
    }
  }

  return totals
})

// 当数组内部变化时（quick-add/remove via splice），仅做防御性校正
watch(
  () => stats.value,
  () => {
    // 如果当前选中的 type 已经没有对应项了，回退到 'all'
    if (selectedType.value !== 'all' && !stats.value[selectedType.value]) {
      selectedType.value = 'all'
    }
    // 如果游戏过滤开启但已无游戏项，关闭过滤
    if (gamesOnly.value && stats.value.games === 0) {
      gamesOnly.value = false
    }
    if (reviewOnly.value && stats.value.review === 0) {
      reviewOnly.value = false
    }
  }
)

// 是否有激活的过滤器
const hasActiveFilter = computed(() => searchQuery.value || gamesOnly.value || reviewOnly.value || selectedType.value !== 'all')

// 过滤后的应用列表
const filteredAppItems = computed(() => {
  const items = []
  const type = selectedType.value
  const query = searchQuery.value ? searchQuery.value.toLowerCase() : ''

  for (const [index, app] of props.apps.entries()) {
    if (type !== 'all' && app['app-type'] !== type) {
      continue
    }
    if (gamesOnly.value && app['is-game'] !== true) {
      continue
    }
    const review = needsReview(app)
    if (reviewOnly.value && !review) {
      continue
    }
    if (query) {
      const name = (app.name || '').toLowerCase()
      const cmd = (app.cmd || '').toLowerCase()
      const sourcePath = (app.source_path || '').toLowerCase()
      if (!name.includes(query) && !cmd.includes(query) && !sourcePath.includes(query)) {
        continue
      }
    }

    items.push({
      app,
      index,
      review,
      reviewReasons: review ? getReviewReasons(app) : [],
    })
  }

  return items
})

// 应用类型标签
function hasNumericValue(value) {
  if (value === null || value === undefined) return false
  if (typeof value === 'string' && value.trim() === '') return false
  return Number.isFinite(Number(value))
}

function needsReview(app) {
  return needsGameResourceReview(app, { locale: locale.value })
}

function getReviewReasons(app) {
  return getGameResourceReviewReasons(app, { locale: locale.value })
}

const getAppTypeLabel = (appType) => {
  const typeMap = {
    executable: t('apps.scan_result_type_executable'),
    shortcut: t('apps.scan_result_type_shortcut'),
    batch: t('apps.scan_result_type_batch'),
    command: t('apps.scan_result_type_command'),
    url: t('apps.scan_result_type_url'),
    steam: 'Steam',
    epic: 'Epic Games',
    gog: 'GOG',
  }
  return typeMap[appType] || appType
}

// 应用类型徽章样式
const getAppTypeBadgeClass = (appType) => {
  const classMap = {
    executable: 'bg-primary',
    shortcut: 'bg-info',
    batch: 'bg-warning text-dark',
    command: 'bg-warning text-dark',
    url: 'bg-success',
    steam: 'bg-primary',
    epic: 'bg-dark',
    gog: 'bg-secondary',
  }
  return classMap[appType] || 'bg-secondary'
}
</script>
