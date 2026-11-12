<template>
  <MSheet class="scan-sheet" :model-value="show" :title="t('apps.scan_result_title')" side="end" :close-label="t('_common.close')" @update:model-value="(open) => !open && $emit('close')">
    <div class="scan-result">
      <div class="scan-result__counts">
        <MChip>{{ apps.length }}</MChip>
        <MChip v-if="stats.games > 0" tone="warn" icon="sports_esports">{{ stats.games }}</MChip>
        <MChip v-if="hasActiveFilter" tone="info">{{ t('apps.scan_result_matched', { count: filteredAppItems.length }) }}</MChip>
      </div>

      <template v-if="apps.length > 0">
        <label class="m-filterbar scan-result__search">
          <MIcon name="search" />
          <input v-model="searchQuery" type="search" :placeholder="t('apps.scan_result_search_placeholder')" :aria-label="t('apps.scan_result_search_placeholder')" autocomplete="off" spellcheck="false" />
          <button v-if="searchQuery" type="button" class="m-filterbar__clear" :aria-label="t('apps.clear_search')" :data-tip="t('apps.clear_search')" @click="searchQuery = ''"><MIcon name="close" /></button>
        </label>

        <div class="scan-result__filters">
          <MSeg v-model="typeFilter" :label="t('apps.scan_result_filter_all')" :options="typeOptions" />
          <MButton v-if="stats.games > 0" :variant="gamesOnly ? 'filled' : 'tonal'" size="sm" icon="sports_esports" @click="gamesOnly = !gamesOnly">{{ gamesOnly ? t('apps.scan_result_show_all') : t('apps.scan_result_games_only') }} {{ stats.games }}</MButton>
          <MButton v-if="stats.review > 0" :variant="reviewOnly ? 'filled' : 'tonal'" size="sm" icon="warning" :aria-pressed="reviewOnly ? 'true' : 'false'" @click="reviewOnly = !reviewOnly">{{ reviewLabel }} {{ stats.review }}</MButton>
        </div>
      </template>

      <div v-if="enhancementProgress.active" class="scan-result__progress" role="status" aria-live="polite">
        <span>{{ enhancementProgress.stage }}<small v-if="enhancementProgress.detail"> {{ enhancementProgress.detail }}</small><b v-if="enhancementProgress.total"> {{ enhancementProgress.current }}/{{ enhancementProgress.total }}</b></span>
        <MProgress :value="enhancementProgress.indeterminate ? null : enhancementProgressPercent / 100" :label="enhancementProgress.stage" />
      </div>

      <MEmpty v-if="apps.length === 0" icon="folder_open" :title="t('apps.scan_result_no_apps')" />
      <MEmpty v-else-if="filteredAppItems.length === 0" icon="search" :title="t('apps.scan_result_no_matches')" />
      <div v-if="apps.length > 0" class="scan-result__select" role="toolbar">
        <span><MIcon name="checklist" />{{ t('apps.scan_result_selected', { n: selectedApps.length, total: apps.length }) }}</span>
        <MButton variant="text" size="sm" icon="select_all" @click="selectAll">{{ t('apps.batch_select_all') }}</MButton>
        <MButton variant="text" size="sm" icon="deselect" :disabled="!selectedApps.length" @click="selected = new Set()">{{ t('apps.batch_clear') }}</MButton>
      </div>
      <!-- one group per source (Steam, Epic, the launchers, a folder, Sparxie's own profiles), each with its own select-all -->
      <template v-for="group in groups" :key="group.source">
      <div class="scan-group__head">
        <b>{{ sourceName(group.source) }}</b><MChip>{{ group.items.length }}</MChip>
        <span class="scan-group__fill" />
        <MButton variant="text" size="sm" :disabled="!group.pickable" @click="toggleGroup(group)">{{ group.allPicked ? t('apps.batch_clear') : t('apps.batch_select_all') }}</MButton>
      </div>
      <MGroup class="scan-result__list">
        <div v-for="{ app, index, review, reviewReasons } in group.items" :key="`${app.source_path || app.cmd || app.name}-${index}`" class="m-item scan-row" :class="{ 'scan-row--review': review, 'is-picked': isPicked(app) }" role="listitem">
          <button type="button" class="scan-row__check" :aria-pressed="isPicked(app)" :aria-label="app.name" :disabled="isAdded(app)" @click="toggle(app)"><MIcon :name="isPicked(app) ? 'check_circle' : 'radio_button_unchecked'" :fill="isPicked(app)" /></button>
          <span class="m-item__lead scan-row__art">
            <img v-if="app['image-path']" :src="app['image-path']" :alt="app.name" loading="lazy" decoding="async" @error="$event.target.style.display = 'none'" />
            <b v-else>{{ app.name.charAt(0).toUpperCase() }}</b>
          </span>
          <span class="m-item__body">
            <span class="m-item__title">{{ app.name }}</span>
            <span class="scan-row__chips">
              <MChip v-if="isAdded(app)" tone="ok" icon="check">{{ spx.scanAdded }}</MChip>
              <MChip v-if="gameOf(app)" tone="tertiary" icon="sports_esports">{{ spx.scanGameApp }}</MChip>
              <MChip v-if="app['app-type'] && app['app-type'] !== 'sparxie'">{{ getAppTypeLabel(app['app-type']) }}</MChip>
              <MChip v-if="app.region">{{ app.region }}</MChip>
              <button v-for="choice in app.launches || []" :key="choice.id" type="button" class="scan-row__mode" :aria-pressed="app.mode === choice.id" @click="app.mode = choice.id">{{ launchLabel(choice.id) }}</button>
              <MChip v-if="app['is-game']" tone="warn" icon="sports_esports">{{ t('apps.scan_result_game') }}</MChip>
              <MChip v-if="review" tone="error" icon="warning">{{ reviewLabel }}</MChip>
              <MChip v-if="hasNumericValue(app['ai-confidence'])" tone="info" icon="wand_stars">{{ Math.round(app['ai-confidence'] * 100) }}%</MChip>
              <MChip v-if="app['cover-source'] || app['cover-match-name']" icon="image">{{ app['cover-source'] || 'cover' }}<template v-if="app['cover-match-name']">: {{ app['cover-match-name'] }}</template><template v-if="hasNumericValue(app['ai-cover-confidence'])"> {{ Math.round(app['ai-cover-confidence'] * 100) }}%</template></MChip>
            </span>
            <span v-if="app['original-name'] && app['original-name'] !== app.name" class="m-item__text">Original: {{ app['original-name'] }}</span>
            <span v-if="app['canonical-name']" class="m-item__text">{{ app['canonical-name'] }}</span>
            <span v-if="review" class="m-item__text scan-row__review">{{ reviewReasons.join(' / ') }}</span>
            <span v-if="app.cmd" class="m-item__text scan-row__cmd">{{ app.cmd }}</span>
            <span v-if="app.source_path" class="m-item__text scan-row__cmd">{{ app.source_path }}</span>
          </span>
          <span class="scan-row__actions">
            <MIconButton v-if="!app.sparxieProfile" icon="edit" :disabled="isAdded(app)" :label="t('apps.scan_result_edit_title')" @click="$emit('edit', app)" />
            <MIconButton icon="add" tonal :disabled="isAdded(app)" :label="t('apps.scan_result_quick_add_title')" @click="$emit('quick-add', app, index)" />
            <MIconButton v-if="!app.sparxieProfile" icon="close" :label="t('apps.scan_result_remove_title')" @click="$emit('remove', index)" />
          </span>
        </div>
      </MGroup>
      </template>
    </div>
    <template v-if="apps.length > 0" #actions>
      <MButton variant="text" @click="$emit('close')">{{ t('_common.cancel') }}</MButton>
      <MButton class="scan-pill" icon="add" :loading="saving" :disabled="!selectedApps.length" @click="$emit('add-selected', selectedApps)">{{ t('apps.scan_result_add_selected', { n: selectedApps.length }) }}</MButton>
    </template>
  </MSheet>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useSparxieCopy } from '../composables/useSparxieCopy.js'
import { MButton, MChip, MEmpty, MGroup, MIcon, MIconButton, MProgress, MSeg, MSheet } from 'foundation-ui/vue'
import {
  getGameResourceReviewReasons,
  needsGameResourceReview,
} from '../utils/agents/gameLibrary/gameLibraryCuratorAgent.js'

const { t, locale } = useI18n()
const spx = useSparxieCopy()

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
  /** an item that is already in the list of applications (it is marked and cannot be picked again) */
  isAdded: { type: Function, default: () => false },
  /** the game app an item would become, or null (an ordinary application): { game, region, name } */
  gameOf: { type: Function, default: () => null },
})

defineEmits(['close', 'edit', 'quick-add', 'remove', 'add-selected'])

// the applications that will be added: all of them to begin with, the person can take some out (or clear and pick a few)
const keyOf = (app) => app.source_path || `${app.name}|${app.cmd}`
const selected = ref(new Set())
const isPicked = (app) => selected.value.has(keyOf(app))
const toggle = (app) => { const next = new Set(selected.value); if (next.has(keyOf(app))) next.delete(keyOf(app)); else next.add(keyOf(app)); selected.value = next }
const selectedApps = computed(() => props.apps.filter((app) => selected.value.has(keyOf(app)) && !props.isAdded(app)))
const selectAll = () => { selected.value = new Set(props.apps.filter((app) => !props.isAdded(app)).map(keyOf)) }
watch(() => props.apps, (list, before) => { const known = new Set((before || []).map(keyOf)); const next = new Set([...selected.value].filter((k) => list.some((a) => keyOf(a) === k))); for (const app of list) if (!known.has(keyOf(app)) && !props.isAdded(app)) next.add(keyOf(app)); selected.value = next }, { immediate: true })

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
    xbox: 0,
    ea: 0,
    ubisoft: 0,
    battlenet: 0,
    riot: 0,
    hoyoplay: 0,
    wegame: 0,
    sparxie: 0,
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

// The type filter as one connected button group; scripts (batch and command files) share a button
const typeOptions = computed(() => [
  { value: 'all', label: `${t('apps.scan_result_filter_all')} ${stats.value.all}` },
  ...(stats.value.shortcut > 0 ? [{ value: 'shortcut', label: `${t('apps.scan_result_filter_shortcut')} ${stats.value.shortcut}`, icon: 'link' }] : []),
  ...(stats.value.executable > 0 ? [{ value: 'executable', label: `${t('apps.scan_result_filter_executable')} ${stats.value.executable}`, icon: 'play_arrow' }] : []),
  ...(stats.value.batch > 0 || stats.value.command > 0 ? [{ value: 'script', label: `${t('apps.scan_result_filter_script')} ${stats.value.batch + stats.value.command}`, icon: 'terminal' }] : []),
  ...(stats.value.url > 0 ? [{ value: 'url', label: `${t('apps.scan_result_filter_url')} ${stats.value.url}`, icon: 'public' }] : []),
  ...(stats.value.steam > 0 ? [{ value: 'steam', label: `Steam ${stats.value.steam}`, icon: 'cloud' }] : []),
  ...(stats.value.epic > 0 ? [{ value: 'epic', label: `Epic ${stats.value.epic}`, icon: 'sports_esports' }] : []),
  ...(stats.value.gog > 0 ? [{ value: 'gog', label: `GOG ${stats.value.gog}`, icon: 'sports_esports' }] : []),
  ...(stats.value.xbox > 0 ? [{ value: 'xbox', label: `Xbox ${stats.value.xbox}`, icon: 'sports_esports' }] : []),
  ...(stats.value.ea > 0 ? [{ value: 'ea', label: `EA ${stats.value.ea}`, icon: 'sports_esports' }] : []),
  ...(stats.value.ubisoft > 0 ? [{ value: 'ubisoft', label: `Ubisoft ${stats.value.ubisoft}`, icon: 'sports_esports' }] : []),
  ...(stats.value.battlenet > 0 ? [{ value: 'battlenet', label: `Battle.net ${stats.value.battlenet}`, icon: 'sports_esports' }] : []),
  ...(stats.value.riot > 0 ? [{ value: 'riot', label: `Riot ${stats.value.riot}`, icon: 'sports_esports' }] : []),
  ...(stats.value.hoyoplay > 0 ? [{ value: 'hoyoplay', label: `HoYoPlay ${stats.value.hoyoplay}`, icon: 'sports_esports' }] : []),
  ...(stats.value.wegame > 0 ? [{ value: 'wegame', label: `WeGame ${stats.value.wegame}`, icon: 'sports_esports' }] : []),
  ...(stats.value.sparxie > 0 ? [{ value: 'sparxie', label: `Sparxie ${stats.value.sparxie}`, icon: 'touch_app' }] : []),
])
const typeFilter = computed({
  get: () => (selectedType.value === 'batch' || selectedType.value === 'command' ? 'script' : selectedType.value),
  set: (value) => { selectedType.value = value === 'script' ? (stats.value.batch > 0 ? 'batch' : 'command') : value },
})

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

// the groups of the list: one per source, in the order of the launchers a person knows; a folder scan is its own group
const SOURCES = ['steam', 'epic', 'gog', 'xbox', 'ea', 'ubisoft', 'battlenet', 'riot', 'hoyoplay', 'wegame', 'sparxie']
const sourceOfApp = (app) => (SOURCES.includes(app['app-type']) ? app['app-type'] : 'directory')
const sourceName = (source) => spx.value.scanSource[source] || source
const groups = computed(() => {
  const by = new Map()
  for (const item of filteredAppItems.value) {
    const source = sourceOfApp(item.app)
    if (!by.has(source)) by.set(source, [])
    by.get(source).push(item)
  }
  return [...by.keys()].sort((a, b) => (SOURCES.indexOf(a) + 1 || 99) - (SOURCES.indexOf(b) + 1 || 99)).map((source) => {
    const items = by.get(source)
    const pickable = items.filter(({ app }) => !props.isAdded(app))
    return { source, items, pickable: pickable.length > 0, allPicked: pickable.length > 0 && pickable.every(({ app }) => isPicked(app)) }
  })
})
const toggleGroup = (group) => {
  const next = new Set(selected.value)
  const items = group.items.filter(({ app }) => !props.isAdded(app))
  for (const { app } of items) { if (group.allPicked) next.delete(keyOf(app)); else next.add(keyOf(app)) }
  selected.value = next
}

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

const launchLabel = (id) => {
  const key = id === 'touch' ? 'apps.scan_launch_touch' : id === 'pc' ? 'apps.scan_launch_pc' : ''
  if (!key) return id
  const text = t(key)
  return text && text !== key ? text : id
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
    xbox: 'Xbox',
    ea: 'EA app',
    ubisoft: 'Ubisoft Connect',
    battlenet: 'Battle.net',
    riot: 'Riot Client',
    hoyoplay: 'HoYoPlay',
    wegame: 'WeGame',
    sparxie: 'Sparxie',
  }
  return typeMap[appType] || appType
}

</script>

<style scoped lang="less">
.scan-result {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.scan-result__counts,
.scan-result__filters,
.scan-row__chips {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
}

.scan-result__progress {
  display: flex;
  flex-direction: column;
  gap: 6px;
  font: var(--m-body-medium);
}

.scan-group__head { display: flex; align-items: center; gap: 8px; padding: 8px 4px 0; }
.scan-group__head b { font: var(--m-title-small); }
.scan-group__fill { flex: 1; }
.scan-row {
  align-items: flex-start;
}

.scan-row__art {
  width: 48px;
  height: 48px;
  border-radius: 12px;
  overflow: hidden;
  display: grid;
  place-items: center;
  background: var(--md-sys-color-primary-container);
  color: var(--md-sys-color-on-primary-container);
  font: var(--m-title-large-emphasized);

  img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }
}

.scan-row__mode {
  border: 0;
  border-radius: 999px;
  padding: 0 8px;
  background: transparent;
  color: inherit;
}
.scan-row__mode[aria-pressed='true'] {
  background: color-mix(in srgb, currentColor 12%, transparent);
}
.scan-row__cmd {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-size: 0.75rem;
}

.scan-row__review {
  color: var(--md-sys-color-error);
}

.scan-row__actions {
  display: inline-flex;
  flex: none;
}

.scan-pill {
  flex: 1;
  min-height: 56px;
  border-radius: 28px;
}
</style>
