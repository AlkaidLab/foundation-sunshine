<template>
  <div class="apps-page">
    <MPage v-show="!pageOpen" wide :title="$t('apps.applications_title')">
      <template #actions>
        <MOverflow :items="menuItems" :label="$t('shell.more')" @select="onMenu" />
      </template>

      <Transition name="fade">
        <div v-if="scanProgress.active" class="apps-scan" role="status" aria-live="polite">
          <div class="apps-scan__copy">
            <MIcon name="wand_stars" />
            <span>{{ scanProgress.stage }}</span>
            <small v-if="scanProgress.detail">{{ scanProgress.detail }}</small>
            <b v-if="scanProgress.total">{{ scanProgress.current }}/{{ scanProgress.total }}</b>
          </div>
          <MProgress :value="scanProgress.indeterminate ? null : scanProgressPercent / 100" :label="scanProgress.stage" />
        </div>
      </Transition>

      <div class="apps-tools">
        <label class="m-filterbar apps-filter">
          <MIcon name="search" />
          <input v-model="searchQuery" type="search" :placeholder="$t('apps.search_placeholder')" :aria-label="$t('apps.search_placeholder')" autocomplete="off" spellcheck="false">
          <button v-if="searchQuery" type="button" class="m-filterbar__clear" :aria-label="$t('apps.clear_search')" :data-tip="$t('apps.clear_search')" @click="clearSearch"><MIcon name="close" /></button>
        </label>
        <MSeg v-model="filterMode" :label="$t('apps.filter_label')" :options="[{ value: 'all', label: $t('apps.filter_all') }, { value: 'running', label: $t('apps.filter_running') }]" />
        <MSeg v-model="sortMode" :label="$t('apps.sort_label')" :options="[{ value: 'custom', label: $t('apps.sort_custom') }, { value: 'asc', label: $t('apps.sort_asc') }, { value: 'desc', label: $t('apps.sort_desc') }]" />
        <MSeg v-model="viewMode" icon-only :label="$t('apps.applications_title')" :options="[{ value: 'grid', label: $t('apps.grid_view'), icon: 'grid_view' }, { value: 'list', label: $t('apps.list_view'), icon: 'list' }]" />
      </div>

      <div v-if="selectionMode" class="apps-batch" role="toolbar" :aria-label="$t('apps.batch_select_toggle')">
        <span class="apps-batch__count"><MIcon name="checklist" />{{ $t('apps.batch_selected', { count: selectedIndices.size }) }}</span>
        <MButton variant="text" size="sm" icon="select_all" @click="selectAllFiltered">{{ $t('apps.batch_select_all') }}</MButton>
        <MButton variant="text" size="sm" icon="deselect" :disabled="selectedIndices.size === 0" @click="clearSelection">{{ $t('apps.batch_clear') }}</MButton>
        <MButton variant="danger" size="sm" icon="delete" :disabled="selectedIndices.size === 0 || isBatchDeleting" @click="askBatchDelete">{{ $t('apps.batch_delete') }}</MButton>
        <MButton variant="text" size="sm" @click="toggleSelectionMode">{{ $t('_common.cancel') }}</MButton>
      </div>

      <MSkeleton v-if="!ready" :rows="4" />

      <template v-else>
        <MCoverGrid
          v-if="viewMode === 'grid' && shownCards.length"
          :items="shownCards"
          :min="176"
          :running-label="$t('apps.running')"
          :selected="selectionMode ? selectedIds : null"
          :reorderable="canReorder"
          @select="onCardSelect"
          @context="onCardMenu"
          @action="onCardAction"
          @reorder="onReorder"
          @image-error="(event, item) => broken.add(item.id)"
        >
          <template #meta="{ item }">
            <MChipRow v-if="item.chips?.length" :chips="item.chips" :more-label="spxCopy.moreChips" />
          </template>
          <template #badge="{ item }">
            <span v-if="item.tags.length" class="m-cover__badges">
              <i v-for="tag in item.tags" :key="tag.key" class="apps-tag" :data-tip="tag.title"><MIcon :name="tag.icon" /><b v-if="tag.count">{{ tag.count }}</b></i>
            </span>
          </template>
        </MCoverGrid>

        <MGroup v-else-if="viewMode === 'list' && shownCards.length" class="apps-list m-segrows">
          <div
            v-for="card in shownCards"
            :key="card.id"
            class="m-item m-split-item apps-row"
            :data-row="card.title"
            :class="{ 'is-selected': selectionMode && selectedIds.includes(card.id), 'is-over': dragOver === card.id && dragFrom !== card.id }"
            role="listitem"
            :draggable="canReorder || undefined"
            @dragstart="dragFrom = card.id"
            @dragover.prevent="dragOver = card.id"
            @drop.prevent="onRowDrop(card.id)"
            @dragend="dragFrom = dragOver = null"
            @contextmenu="onCardMenu($event, card)"
          >
            <button type="button" class="m-split-item__main apps-row__main" @click="onCardSelect(card)" @keydown="onRowKey($event, card)">
              <span class="m-item__lead apps-row__art">
                <img v-if="card.cover" :src="card.cover" alt="" loading="lazy" decoding="async" @error="broken.add(card.id)"><MIcon v-else name="apps" />
              </span>
              <span class="m-item__body">
                <span class="m-item__title">{{ card.title }}<i v-if="card.running" class="apps-live"><b aria-hidden="true" />{{ $t('apps.running') }}</i></span>
                <span class="m-item__text apps-row__cmd">{{ card.sub }}</span>
              </span>
              <span v-if="card.tags.length" class="apps-row__tags">
                <i v-for="tag in card.tags" :key="tag.key" class="apps-tag" :data-tip="tag.title"><MIcon :name="tag.icon" /><b v-if="tag.count">{{ tag.count }}</b></i>
              </span>
              <MIcon v-if="selectionMode" class="apps-row__check" :name="selectedIds.includes(card.id) ? 'check_circle' : 'radio_button_unchecked'" :fill="selectedIds.includes(card.id)" />
            </button>
            <i class="m-split-item__divider" aria-hidden="true" />
            <button type="button" class="m-split-item__gear" :aria-label="$t('shell.more')" :data-tip="$t('shell.more')" @click="onCardMenu($event, card)"><MIcon name="more_vert" /></button>
          </div>
        </MGroup>

        <MEmpty v-else-if="filterMode === 'running' && !searchQuery" icon="play_circle" :title="$t('apps.empty_running')" />
        <MEmpty v-else-if="searchQuery" icon="search" :title="$t('apps.scan_result_no_matches')" />
        <MEmpty v-else icon="apps" :title="$t('apps.no_apps')">
          <MButton icon="add" @click="newApp">{{ $t('apps.add_new') }}</MButton>
          <MButton v-if="canScan()" variant="tonal" icon="sports_esports" @click="openScanOptions">{{ $t('apps.scan_games') }}</MButton>
        </MEmpty>
      </template>

      <MFooter v-if="ready && apps.length">{{ $t('apps.applications_desc') }}</MFooter>
      <MSceneFab v-if="isLoaded">
        <!-- 添加: one FAB; where the host can scan, a FAB menu of 手动添加 and 扫描游戏库 (M3 Expressive FAB menu in place of two stacked FABs) -->
        <MFabMenu v-if="canScan() || spxAvailable" icon="add" :label="$t('apps.add_new')" :items="fabItems" @select="onFab" />
        <MFab v-else class="apps-fab" extended icon="add" :label="$t('apps.add_new')" @click="newApp" />
      </MSceneFab>
    </MPage>

    <!-- Application editor: a page of the area (back arrow, poster, groups, save bar); the list waits behind it with its scroll position -->
    <AppEditor
      v-if="editingApp && !isGameApp(editingApp)"
      :app="editingApp"
      :platform="platform"
      :disabled="isSaving"
      @save-app="handleSaveApp"
      @close="closeAppEditor"
      @to-game="takeOverToGame"
    />
    <!-- a game app opens the page 添加游戏 in its editing state -->
    <SparxieNewApp v-if="editingApp && isGameApp(editingApp)" :edit="editingApp" :back-label="$t('apps.applications_title')" @close="closeAppEditor" @saved="onGameSaved" />

    <ScanResultModal
      :show="showScanResult"
      :apps="scanList"
      :is-added="isScanAdded"
      :game-of="gameOfScan"
      :saving="isSaving"
      :enhancement-progress="scanProgress"
      :enhancement-progress-percent="scanProgressPercent"
      @close="closeScanResult"
      @edit="handleScanEdit"
      @quick-add="(app) => onScanAdd([app])"
      @remove="removeScannedApp"
      @add-selected="onScanAdd"
    />

    <SparxieNewApp v-if="spxAvailable && spxPage === 'new'" :prefill="spxPrefill" :back-label="$t('apps.applications_title')" @close="spxPage = ''" @created="onSpxCreated" />

    <!-- Library scan options -->
    <MSheet :model-value="showScanOptions" :title="$t('apps.scan_options.title')" side="center" :close-label="$t('_common.close')" @update:model-value="(open) => !open && closeScanOptions()">
      <div class="apps-sheet">
        <MSeg
          v-model="scanOptions.scope"
          block
          :label="$t('apps.scan_options.scope')"
          :options="[{ value: 'libraries', label: $t('apps.scan_options.libraries'), icon: 'sports_esports' }, { value: 'directory', label: $t('apps.scan_options.directory'), icon: 'folder_open' }]"
        />
        <MFooter v-if="scanOptions.scope === 'libraries'">{{ $t('apps.scan_options.libraries_desc') }}</MFooter>

        <MSection v-if="scanOptions.scope === 'libraries'" :title="$t('apps.scan_options.platforms')">
          <MGroup>
            <MSwitchItem v-for="platformOption in scanPlatformOptions" :key="platformOption.id" v-model="scanOptions.platforms[platformOption.id]" :icon="platformOption.icon" :title="platformOption.label" />
          </MGroup>
        </MSection>

        <MSection v-if="scanOptions.scope === 'directory'" :title="$t('apps.scan_options.directory_scan')">
          <MGroup>
            <MSwitchItem v-if="isTauriEnv()" v-model="scanOptions.extractIcons" :title="$t('apps.scan_options.extract_icons')" :text="$t('apps.scan_options.extract_icons_desc')" />
          </MGroup>
          <MField v-if="!isTauriEnv()" v-model="scanOptions.directory" :label="$t('apps.scan_options.directory_path')" placeholder="D:\\Games" />
          <MFooter v-if="!isTauriEnv()">{{ $t('apps.scan_options.directory_host_note') }}</MFooter>
        </MSection>

        <MSection :title="$t('apps.scan_options.ai')">
          <MGroup>
            <MSwitchItem
              v-for="skill in selectableGameLibrarySkills"
              :key="skill.skillId"
              :model-value="isGameLibrarySkillEnabled(skill.skillId)"
              :title="getGameLibrarySkillLabel(skill.skillId)"
              @update:model-value="toggleGameLibrarySkill(skill.skillId)"
            />
          </MGroup>
        </MSection>
      </div>
      <template #actions>
        <MButton variant="text" :disabled="isScanning" @click="closeScanOptions">{{ $t('_common.cancel') }}</MButton>
        <MButton class="apps-pill" :icon="isScanning ? '' : 'search'" :loading="isScanning" @click="runConfiguredScan">{{ $t('apps.scan_options.start') }}</MButton>
      </template>
    </MSheet>

    <!-- Environment variables of the commands -->
    <MSheet v-model="showEnv" :title="$t('apps.env_vars_about')" side="end" :close-label="$t('_common.close')">
      <div class="apps-sheet">
        <p class="apps-sheet__lead">{{ $t('apps.env_vars_desc') }}</p>
        <MTable
          dense
          row-key="name"
          :columns="[{ key: 'name', label: $t('apps.env_var_name') }, { key: 'desc', label: $t('_common.description') }]"
          :rows="envRows"
        >
          <template #cell-name="{ value }"><code class="apps-code">{{ value }}</code></template>
        </MTable>
        <template v-if="envExample">
          <b class="apps-sheet__eg">{{ envExample.title }}</b>
          <pre class="apps-pre">{{ envExample.code }}</pre>
        </template>
      </div>
      <template #actions>
        <a class="m-btn m-btn--outlined" href="https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2app__examples.html" target="_blank" rel="noopener noreferrer"><MIcon name="open_in_new" />{{ $t('_common.see_more') }}</a>
        <MButton variant="tonal" @click="showEnv = false">{{ $t('_common.close') }}</MButton>
      </template>
    </MSheet>

    <!-- Delete -->
    <MDialog :model-value="deleteConfirmIndex !== null" :title="$t('_common.delete')" icon="delete" tone="error" @update:model-value="(open) => !open && cancelDeleteApp()">
      <p>{{ $t('apps.delete_confirm', { name: apps[deleteConfirmIndex]?.name || '' }) }}</p>
      <template #actions>
        <MButton variant="text" @click="cancelDeleteApp">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" icon="delete" @click="confirmDeleteApp">{{ $t('_common.delete') }}</MButton>
      </template>
    </MDialog>
    <MDialog :model-value="batchDeleteConfirm" :title="$t('apps.batch_delete')" icon="delete" tone="error" @update:model-value="(open) => !open && cancelBatchDelete()">
      <p>{{ $t('apps.batch_delete_confirm', { count: selectedIndices.size }) }}</p>
      <template #actions>
        <MButton variant="text" :disabled="isBatchDeleting" @click="cancelBatchDelete">{{ $t('_common.cancel') }}</MButton>
        <MButton variant="danger" icon="delete" :loading="isBatchDeleting" @click="confirmBatchDelete">{{ $t('_common.delete') }}</MButton>
      </template>
    </MDialog>

    <MSnackbar :model-value="!!message" :message="message" @update:model-value="(open) => !open && (message = '')" />
  </div>
</template>

<script setup>
import { computed, inject, reactive, ref, onBeforeUnmount, onMounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { hostData } from '../composables/hostData.js'
import { deepClone } from '../utils/helpers.js'
import { MButton, MChipRow, MCoverGrid, MDialog, MEmpty, MFab, MFabMenu, MField, MFooter, MGroup, MIcon, MOverflow, MPage, MProgress, MSceneFab, MSection, MSeg, MSheet, MSkeleton, MSnackbar, MSwitchItem, MTable, usePageFilter } from 'foundation-ui/vue'
import ScanResultModal from '../components/ScanResultModal.vue'
import AppEditorLoading from '../components/AppEditorLoading.vue'
import { lazyPane } from '../utils/lazyPane.js'
import { useApps } from '../composables/useApps.js'
import { useAppsPending } from '../composables/pendingApps.js'
import { getImagePreviewUrl } from '../utils/imageUtils.js'
import { appTags, envExampleFor, moveApp, sortCards } from '../utils/appsPage.js'
import { apiJson, apiPostJson } from '../utils/apiFetch.js'
import SparxieNewApp from '../components/sparxie/SparxieNewApp.vue'
import { AppService } from '../services/appService.js'
import { sparxieClient, useSparxie } from '../composables/useSparxie.js'
import { useSparxieCopy } from '../composables/useSparxieCopy.js'
import { appChips, artUrl, sparxieOf } from '../utils/sparxie/model.js'

const { t, locale } = useI18n()

// the page is there with its title and back arrow the moment the button is pressed; its form comes when the chunk does
const AppEditor = lazyPane(() => import('../components/AppEditor.vue'), { loading: AppEditorLoading })

const isLoaded = ref(false)
// the page draws from the last answer of the host at once (hostData.peek) and the fresh one replaces it in place: the skeleton is only for a first start
const keptApps = hostData.peek('apps')
const hasKept = ref(!!(keptApps && Array.isArray(keptApps.apps)))
const ready = computed(() => isLoaded.value || hasKept.value)

// the Sparxie component: two more entries in the add menu while it is installed; without it the menu is the page's own
const { present: spxPresent, available: spxAvailable, refresh: refreshSparxie } = useSparxie()
const spxCopy = useSparxieCopy()
const spxPage = ref('')
const fabItems = computed(() => {
  // three entries: the library scan (every source, Sparxie's own profiles among them), 添加游戏 (the game apps), 添加应用 (any program)
  const items = []
  if (canScan()) items.push({ id: 'scan', icon: 'sports_esports', label: t('apps.scan_games') })
  if (spxAvailable.value) items.push({ id: 'spx-new', icon: 'add_circle', label: spxCopy.value.addGame })
  items.push({ id: 'add', icon: 'edit_note', label: spxCopy.value.addApp })
  return items
})
const pageOpen = computed(() => !!editingApp.value || !!spxPage.value)
const spxPrefill = ref('')
const onFab = (item) => { if (item.id === 'scan') openScanOptions(); else if (item.id === 'spx-new') { spxPrefill.value = ''; spxPage.value = 'new' } else newApp() }
// 添加应用 found that the program is a game that has its own way to be added: the path goes with the person to 添加游戏
const takeOverToGame = (path) => { closeAppEditor(); spxPrefill.value = path; spxPage.value = 'new' }
const isGameApp = (app) => spxAvailable.value && !!sparxieOf(app) && Number.isInteger(app.index) && app.index >= 0
const onGameSaved = async () => { closeAppEditor(); await loadApps(); message.value = t('apps.feedback.save_success') }

const {
  apps,
  filteredApps,
  filteredAppsWithIndex,
  searchQuery,
  editingApp,
  platform,
  isSaving,
  originalApps,
  viewMode,
  message,
  envVars,
  debouncedSearch,
  isScanning,
  scanProgress,
  scanProgressPercent,
  scannedApps,
  showScanResult,
  showScanOptions,
  scanOptions,
  scanPlatformOptions,
  selectableGameLibrarySkills,
  loadApps,
  loadPlatform,
  clearSearch,
  getAppRenderKey,
  reorderApps,
  newApp,
  editApp,
  closeAppEditor,
  handleSaveApp,
  showDeleteForm,
  cancelDeleteApp,
  confirmDeleteApp,
  deleteConfirmIndex,
  selectionMode,
  selectedIndices,
  batchDeleteConfirm,
  isBatchDeleting,
  toggleSelectionMode,
  toggleAppSelection,
  selectAllFiltered,
  clearSelection,
  askBatchDelete,
  cancelBatchDelete,
  confirmBatchDelete,
  save,
  hasUnsavedChanges,
  restoreDefaultApps,
  openScanOptions,
  closeScanOptions,
  runConfiguredScan,
  addScannedApp,
  addAllScannedApps,
  closeScanResult,
  removeScannedApp,
  quickAddScannedApp,
  isTauriEnv,
  canScan,
  isGameLibrarySkillEnabled,
  toggleGameLibrarySkill,
  getGameLibrarySkillLabel,
  init,
} = useApps()

// the edits of the list belong to the window's unsaved state (one bar, one save), so they stay when the person goes to another page
useAppsPending({ apps, originalApps, locale: () => locale.value, loaded: isLoaded })
const webShell = inject('webShell', null)
const showEnv = ref(false)
const broken = reactive(new Set())
const dragFrom = ref(null)
const dragOver = ref(null)

// the cover of a card: a Sparxie application that follows the official poster shows the 2:3 cover of the host's art cache (its version is in the URL); any other application shows its own image
const coverOf = (app) => {
  const block = spxAvailable.value ? sparxieOf(app) : null
  if (block && block.cover !== 'custom' && block.install) return artUrl('card', { id: block.install }, block.artVersion)
  return app['image-path'] && !broken.has(getAppRenderKey(app)) ? getImagePreviewUrl(app['image-path']) : ''
}

// One card per application; the id is the render key, the index is the position in the full list.
const cards = computed(() =>
  filteredAppsWithIndex.value.map(({ app, index }) => ({
    id: getAppRenderKey(app),
    index,
    title: app.name,
    text: app.cmd || '',
    // the list view has room for a line under the name: the command, else what the application is (no program: the desktop is streamed)
    sub: app.cmd || app['working-dir'] || t('apps.row_no_command'),
    cover: coverOf(app),
    tags: appTags(app, t),
    chips: spxAvailable.value ? appChips(app, spxCopy.value) : [],
    running: running.value.has(app.name),
    // the buttons over the art on hover: edit, close a running application, the rest of the menu (no launch from here: an application starts from a client)
    actions: [{ id: 'edit', icon: 'edit', label: t('apps.edit') }, ...(running.value.has(app.name) ? [{ id: 'close', icon: 'stop_circle', label: t('apps.close_running') }] : []), { id: 'more', icon: 'more_horiz', label: t('shell.more') }],
  }))
)
// which applications run now (the sessions of the host), refreshed while the page is open
const running = ref(new Set())
let runTimer = 0
async function pollRunning() {
  try { const data = await apiJson('/api/runtime/sessions'); running.value = new Set((data?.sessions || []).map((s) => s.app_name).filter(Boolean)) } catch { running.value = new Set() }
}
onMounted(() => { pollRunning(); runTimer = setInterval(pollRunning, 5000) })
onBeforeUnmount(() => clearInterval(runTimer))
const filterMode = ref('all')
const sortMode = ref('custom')
// the cards as they are shown: filtered by state, then sorted (the saved order is only changed in the custom order)
const shownCards = computed(() => sortCards(filterMode.value === 'running' ? cards.value.filter((c) => c.running) : cards.value, sortMode.value))
async function closeRunning() {
  try { await apiPostJson('/api/apps/close', {}); await pollRunning() } catch { /* the next poll shows the state */ }
}
function onCardAction({ id, item, event }) {
  if (id === 'edit') editApp(item.index)
  else if (id === 'close') closeRunning()
  else if (id === 'more') onCardMenu(event, item)
}
const selectedIds = computed(() => cards.value.filter((card) => selectedIndices.value.has(card.index)).map((card) => card.id))
const canReorder = computed(() => !searchQuery.value && !selectionMode.value && apps.value.length > 1 && filterMode.value === 'all' && sortMode.value === 'custom')

const onCardSelect = (card) => (selectionMode.value ? toggleAppSelection(card.index) : editApp(card.index))

// Moving an application earlier or later: the whole list keeps its order apart from the moved one.
const moveBy = (card, step) => {
  const target = shownCards.value[shownCards.value.findIndex((c) => c.id === card.id) + step]
  if (target) onReorder({ from: card.id, to: target.id })
}
function onReorder({ from, to }) {
  if (!canReorder.value) return
  reorderApps(moveApp(filteredAppsWithIndex.value, (entry) => getAppRenderKey(entry.app), from, to))
}
function onRowDrop(id) {
  if (dragFrom.value && dragFrom.value !== id) onReorder({ from: dragFrom.value, to: id })
  dragFrom.value = dragOver.value = null
}
function onRowKey(event, card) {
  if (!canReorder.value || !event.altKey) return
  const step = event.key === 'ArrowUp' ? -1 : event.key === 'ArrowDown' ? 1 : 0
  if (!step) return
  event.preventDefault()
  moveBy(card, step)
}

async function copyCommand(card) {
  const app = apps.value[card.index]
  if (!app?.cmd) { message.value = t('apps.copy_no_command'); return }
  try {
    await navigator.clipboard.writeText(app.cmd)
    message.value = t('apps.copy_command_success', { name: app.name })
  } catch {
    message.value = t('apps.copy_failed_permissions')
  }
}

// Right click, context-menu key or long press on a card
function onCardMenu(event, card) {
  if (!webShell?.showMenu) return
  const first = shownCards.value[0]?.id === card.id
  const last = shownCards.value[shownCards.value.length - 1]?.id === card.id
  webShell.showMenu(event, [
    { id: 'edit', icon: 'edit', label: t('apps.edit') },
    ...(card.text ? [{ id: 'copy', icon: 'content_copy', label: t('_common.copy') }] : []),
    ...(canReorder.value && !first ? [{ id: 'earlier', icon: 'arrow_upward', label: t('apps.move_earlier') }] : []),
    ...(canReorder.value && !last ? [{ id: 'later', icon: 'arrow_downward', label: t('apps.move_later') }] : []),
    { id: 'select', icon: 'checklist', label: t('apps.batch_select_toggle') },
    { id: 'sep', divider: true },
    { id: 'delete', icon: 'delete', label: t('apps.delete'), danger: true },
  ], card, (entry) => {
    if (entry.id === 'edit') editApp(card.index)
    else if (entry.id === 'copy') copyCommand(card)
    else if (entry.id === 'earlier') moveBy(card, -1)
    else if (entry.id === 'later') moveBy(card, 1)
    else if (entry.id === 'select') { if (!selectionMode.value) toggleSelectionMode(); if (!selectedIndices.value.has(card.index)) toggleAppSelection(card.index) }
    else if (entry.id === 'delete') showDeleteForm(card.index)
  })
}

// Page menu: the secondary actions
const menuItems = computed(() => [
  { id: 'select', icon: 'checklist', label: t('apps.batch_select_toggle') },
  { id: 'restore', icon: 'history', label: t('apps.restore_default_apps') },
  ...(canScan() ? [{ id: 'scan', icon: 'sports_esports', label: t('apps.scan_library') }] : []),
  { id: 'sep', divider: true },
  { id: 'env', icon: 'code', label: t('apps.env_vars_about') },
])
function onMenu(id) {
  if (id === 'select') toggleSelectionMode()
  else if (id === 'restore') restoreDefaultApps()
  else if (id === 'scan') openScanOptions()
  else if (id === 'env') showEnv.value = true
}

const envRows = computed(() => Object.entries(envVars.value || {}).map(([name, desc]) => ({ name, desc })))
const envExample = computed(() => envExampleFor(platform.value, t))

// "Add application" of the list pane's action button, and "scan the game library" of the search: the scan waits for the platform
let scanAfterLoad = false
watch(() => webShell?.pending.value, (request) => {
  if (request !== 'addApp' && request !== 'scanGames') return
  webShell.pending.value = ''
  if (request === 'addApp') newApp()
  else if (isLoaded.value) { if (canScan()) openScanOptions() }
  else scanAfterLoad = true
}, { immediate: true })

// The official posters change now and then (the host refreshes them when it starts and while it runs). Ask it once in a while when the page opens and read the list again when one changed:
// a poster that changed is a new address (its version is part of it), so the cards show the new one and not the one that was cached.
let lastArtCheck = 0
watch(spxAvailable, async (on) => {
  if (!on || Date.now() - lastArtCheck < 10 * 60 * 1000) return
  lastArtCheck = Date.now()
  if (await sparxieClient().refreshArt()) await loadApps()
}, { immediate: true })

// the application a Sparxie page made is saved like any other and the person lands on its own page to adjust it
// the host appends the application (and, for an import, each application) to its list itself: the page only reads the list again
const onSpxCreated = async ({ app, saved }) => {
  spxPage.value = ''
  if (app && saved === false) await handleSaveApp(app)
  else await loadApps()
}

// ---- the library scan: every source in one list, Sparxie's own profiles among them, game apps recognised ----
const normPath = (p) => String(p || '').replace(/"/g, '').replace(/\//g, '\\').replace(/\\+$/, '').toLowerCase()
const addedKeys = computed(() => {
  const dirs = new Set(); const exes = new Set(); const cmds = new Set()
  for (const a of apps.value) {
    if (a['working-dir']) dirs.add(normPath(a['working-dir']))
    if (a.sparxie?.dir) dirs.add(normPath(a.sparxie.dir))
    if (a.sparxie?.exe) exes.add(normPath(a.sparxie.exe))
    if (a.cmd) cmds.add(normPath(a.cmd))
  }
  return { dirs, exes, cmds }
})
const spxRows = ref([])
const gameMap = reactive({})
const scanList = computed(() => [...scannedApps.value, ...spxRows.value])
const isScanAdded = (app) => {
  if (app.sparxieProfile) return !!app.taken
  const k = addedKeys.value
  return (!!app.exe && k.exes.has(normPath(app.exe))) || (!!app.source_path && k.dirs.has(normPath(app.source_path))) || (!!app.cmd && k.cmds.has(normPath(app.cmd)))
}
const scanKey = (app) => app.source_path || `${app.name}|${app.cmd}`
const gameOfScan = (app) => (app.sparxieProfile ? { game: app.game, region: '' } : gameMap[scanKey(app)] || null)
async function loadSpxRows() {
  spxRows.value = []
  if (!spxPresent.value) return
  try {
    const profiles = await sparxieClient().profiles()
    spxRows.value = profiles.map((p) => ({ name: `${spxCopy.value.games[p.install.game] || p.install.game}${p.name ? ` · ${p.name}` : ''}`, 'app-type': 'sparxie', 'is-game': true, sparxieProfile: p.id, taken: p.taken, game: p.install.game, source_path: `sparxie:${p.id}`, cmd: '', 'image-path': artUrl('icon', p.install, p.install.art.version) }))
  } catch { /* the other sources still show */ }
}
// a scanned program whose executable is on the host's list of games is recognised: it becomes a game app when it is added (自动 settings)
let titleCache = null
async function recogniseScan(list) {
  for (const key of Object.keys(gameMap)) delete gameMap[key]
  if (!spxAvailable.value || !list.length) return
  try {
    titleCache = titleCache || (await sparxieClient().games()).catalog
    const names = new Map()
    for (const [id, row] of Object.entries(titleCache)) for (const n of row.exes) names.set(n.toLowerCase(), id)
    for (const app of list) {
      const file = String(app.exe || '').split(/[\\/]/).pop().toLowerCase()
      if (!file || !names.has(file)) continue
      const found = await sparxieClient().identify(app.exe)
      // the region must be decided by the host's markers; an open one stays an ordinary application
      if (found.install?.region) gameMap[scanKey(app)] = { game: found.install.game, region: found.install.region, name: titleCache[found.install.game]?.name || found.install.game }
    }
  } catch { /* nothing is recognised: the programs are added as they are */ }
}
watch(scannedApps, (list) => { recogniseScan(list) })
watch(showScanResult, (on) => { if (on) loadSpxRows(); else spxRows.value = [] })
async function onScanAdd(list) {
  const profiles = list.filter((a) => a.sparxieProfile)
  const games = list.filter((a) => !a.sparxieProfile && gameOfScan(a))
  const rest = list.filter((a) => !a.sparxieProfile && !gameOfScan(a))
  try {
    if (profiles.length) await sparxieClient().importProfiles(profiles.map((a) => a.sparxieProfile))
    for (const a of games) {
      const g = gameOfScan(a)
      const touchOk = spxPresent.value && titleCache?.[g.game]?.touch
      await sparxieClient().createApp({ install: '', path: a.exe, region: g.region, mode: touchOk ? 'touch' : 'pc', name: a.name })
    }
  } catch (e) { message.value = e?.code === 'sparxie_running' ? spxCopy.value.closeSparxie : spxCopy.value.createFailed; return }
  const gone = new Set(games.map(scanKey))
  if (games.length) scannedApps.value = scannedApps.value.filter((a) => !gone.has(scanKey(a)))
  if (rest.length) await addAllScannedApps(rest)
  else { await loadApps(); if (!scannedApps.value.length && !spxRows.value.some((r) => !r.taken)) showScanResult.value = false }
  await loadSpxRows()
}
onMounted(async () => {
  refreshSparxie()
  init(t)
  if (hasKept.value) { apps.value = keptApps.apps; originalApps.value = deepClone(keptApps.apps) }
  await Promise.all([loadApps(), loadPlatform()])
  isLoaded.value = true
  if (scanAfterLoad && canScan()) openScanOptions()
  // The guided first run opens this page with what to do next: #guide-scan, #guide-add or #guide-restore
  const request = /^#guide-(scan|add|restore)$/.exec(window.location.hash)?.[1]
  if (!request) return
  window.history.replaceState(null, '', window.location.pathname + window.location.search)
  if (request === 'add') newApp()
  else if (request === 'restore') restoreDefaultApps()
  else if (canScan()) openScanOptions()
})

// an application chosen in the search: the filter takes its name, the row pulses
usePageFilter({ page: 'apps', apply: (q) => { searchQuery.value = q }, rowSelector: (q) => `[data-row="${CSS.escape(q)}"]` })
watch(searchQuery, (query) => {
  if (query) debouncedSearch.value?.()
  else clearSearch()
})

const handleScanEdit = (app) => {
  addScannedApp(app)
  closeScanResult()
}
</script>
