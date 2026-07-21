<template>
  <div class="apps-page">
    <Navbar />
    <div class="container-fluid px-3">
      <div class="my-4">
        <h1 class="page-title">{{ $t('apps.applications_title') }}</h1>
        <p class="page-subtitle">{{ $t('apps.applications_desc') }}</p>
      </div>

      <!-- 搜索栏和功能按钮 -->
      <div class="search-container mb-4">
        <div class="search-box">
          <i class="fas fa-search search-icon"></i>
          <input
            type="text"
            class="form-control search-input"
            :placeholder="$t('apps.search_placeholder')"
            v-model="searchQuery"
          />
          <button v-if="searchQuery" class="btn-clear-search" @click="clearSearch">
            <i class="fas fa-times"></i>
          </button>
        </div>

        <!-- 功能按钮组 -->
        <div class="action-buttons">
          <div class="view-toggle-group">
            <button
              class="view-toggle-btn"
              :class="{ active: viewMode === 'grid' }"
              @click="viewMode = 'grid'"
              title="网格视图"
            >
              <i class="fas fa-th"></i>
            </button>
            <button
              class="view-toggle-btn"
              :class="{ active: viewMode === 'list' }"
              @click="viewMode = 'list'"
              title="列表视图"
            >
              <i class="fas fa-list"></i>
            </button>
          </div>

          <button class="cute-btn cute-btn-primary" @click="newApp" :title="$t('apps.add_new')">
            <i class="fas fa-plus"></i>
          </button>
          <button
            class="cute-btn cute-btn-secondary"
            type="button"
            @click="restoreDefaultApps"
            :disabled="isSaving"
            :title="$t('apps.restore_default_apps')"
            :aria-label="$t('apps.restore_default_apps')"
          >
            <i class="fas fa-rotate-left"></i>
          </button>
          <button
            class="cute-btn"
            :class="selectionMode ? 'cute-btn-primary' : 'cute-btn-secondary'"
            type="button"
            :aria-pressed="selectionMode"
            :aria-label="$t('apps.batch_select_toggle')"
            @click="toggleSelectionMode"
            :title="$t('apps.batch_select_toggle')"
          >
            <i class="fas" :class="selectionMode ? 'fa-square-check' : 'fa-square'"></i>
          </button>
          <button
            v-if="isTauriEnv()"
            class="cute-btn cute-btn-secondary"
            @click="openScanOptions"
            :disabled="isScanning || scanProgress.active"
            title="扫描游戏平台库 (Steam/Epic/GOG)"
            aria-label="扫描游戏平台库 (Steam/Epic/GOG)"
          >
            <i class="fas" :class="isScanning || scanProgress.active ? 'fa-spinner fa-spin' : 'fa-gamepad'"></i>
          </button>
          <button
            class="cute-btn cute-btn-secondary"
            type="button"
            data-bs-toggle="modal"
            data-bs-target="#envVarsModal"
            :title="$t('apps.env_vars_about')"
            :aria-label="$t('apps.env_vars_about')"
          >
            <i class="fas fa-info-circle"></i>
          </button>
          <button 
            class="cute-btn cute-btn-primary"
            :class="{ 'has-changes': hasUnsavedChanges }"
            @click="save" 
            :disabled="!hasUnsavedChanges || isSaving"
            :title="hasUnsavedChanges ? $t('_common.save') : $t('_common.no_changes')"
          >
            <i class="fas fa-save"></i>
            <span v-if="hasUnsavedChanges" class="unsaved-indicator"></span>
          </button>
        </div>

        <Transition name="fade">
          <div v-if="scanProgress.active" class="scan-progress-strip" role="status" aria-live="polite">
            <div class="scan-progress-copy">
              <i class="fas fa-wand-magic-sparkles"></i>
              <span>{{ scanProgress.stage }}</span>
              <small v-if="scanProgress.detail">{{ scanProgress.detail }}</small>
            </div>
            <div v-if="scanProgress.total" class="scan-progress-count">
              {{ scanProgress.current }}/{{ scanProgress.total }}
            </div>
            <div class="scan-progress-bar" :class="{ 'scan-progress-bar--indeterminate': scanProgress.indeterminate }">
              <span :style="{ width: scanProgress.indeterminate ? '42%' : `${scanProgressPercent}%` }"></span>
            </div>
          </div>
        </Transition>
      </div>

      <!-- 批量操作工具栏 -->
      <Transition name="fade">
        <div v-if="selectionMode" class="batch-action-bar">
          <div class="batch-action-info">
            <i class="fas fa-square-check me-2"></i>
            <span>{{ $t('apps.batch_selected', { count: selectedIndices.size }) }}</span>
          </div>
          <div class="batch-action-buttons">
            <button class="btn btn-sm btn-outline-secondary" @click="selectAllFiltered">
              <i class="fas fa-check-double me-1"></i>{{ $t('apps.batch_select_all') }}
            </button>
            <button
              class="btn btn-sm btn-outline-secondary"
              :disabled="selectedIndices.size === 0"
              @click="clearSelection"
            >
              <i class="fas fa-eraser me-1"></i>{{ $t('apps.batch_clear') }}
            </button>
            <button
              class="btn btn-sm btn-danger"
              :disabled="selectedIndices.size === 0 || isBatchDeleting"
              @click="askBatchDelete"
            >
              <i class="fas fa-trash me-1"></i>{{ $t('apps.batch_delete') }}
            </button>
          </div>
        </div>
      </Transition>

      <!-- 应用卡片列表 -->
      <div class="apps-grid-container">
        <!-- 网格视图 - 拖拽模式 -->
        <draggable
          v-if="viewMode === 'grid' && !searchQuery"
          v-model="apps"
          :item-key="getAppRenderKey"
          class="apps-grid"
          :animation="300"
          handle=".drag-handle"
          :delay="180"
          :delay-on-touch-only="true"
          :touch-start-threshold="8"
          :fallback-tolerance="8"
          :disabled="false"
          ghost-class="app-card-ghost"
          chosen-class="app-card-chosen"
          drag-class="app-card-drag"
          @start="onDragStart"
          @end="onDragEnd"
        >
          <template #item="{ element: app, index }">
            <div class="app-card-wrapper" :class="{ 'selection-mode': selectionMode, 'is-selected': isAppSelected(index) }">
              <div
                v-if="selectionMode"
                class="app-select-checkbox"
                role="checkbox"
                tabindex="0"
                :aria-checked="isAppSelected(index)"
                :aria-label="$t('apps.batch_select_toggle')"
                @click.stop="toggleAppSelection(index)"
                @keydown.space.prevent="toggleAppSelection(index)"
                @keydown.enter.prevent="toggleAppSelection(index)"
              >
                <i class="fas" :class="isAppSelected(index) ? 'fa-square-check' : 'fa-square'"></i>
              </div>
              <AppCard
                :app="app"
                :draggable="!selectionMode"
                :is-dragging="isDragging"
                @edit="selectionMode ? toggleAppSelection(index) : editApp(index)"
                @delete="showDeleteForm(index)"
                @copy-success="handleCopySuccess"
                @copy-error="handleCopyError"
              />
            </div>
          </template>
        </draggable>

        <!-- 网格视图 - 搜索模式 -->
        <div v-else-if="viewMode === 'grid' && searchQuery" class="apps-grid">
          <div
            v-for="({ app, index: originalIndex }, index) in filteredAppsWithIndex"
            :key="`search-grid-${app.name}-${originalIndex}-${index}`"
            class="app-card-wrapper"
            :class="{ 'selection-mode': selectionMode, 'is-selected': isAppSelected(originalIndex) }"
          >
            <div
              v-if="selectionMode"
              class="app-select-checkbox"
              role="checkbox"
              tabindex="0"
              :aria-checked="isAppSelected(originalIndex)"
              :aria-label="$t('apps.batch_select_toggle')"
              @click.stop="toggleAppSelection(originalIndex)"
              @keydown.space.prevent="toggleAppSelection(originalIndex)"
              @keydown.enter.prevent="toggleAppSelection(originalIndex)"
            >
              <i class="fas" :class="isAppSelected(originalIndex) ? 'fa-square-check' : 'fa-square'"></i>
            </div>
            <AppCard
              :app="app"
              :draggable="false"
              :is-search-result="true"
              :is-dragging="false"
              @edit="selectionMode ? toggleAppSelection(originalIndex) : editApp(originalIndex)"
              @delete="showDeleteForm(originalIndex)"
              @copy-success="handleCopySuccess"
              @copy-error="handleCopyError"
            />
          </div>
        </div>

        <!-- 列表视图 - 拖拽模式 -->
        <draggable
          v-else-if="viewMode === 'list' && !searchQuery"
          v-model="apps"
          :item-key="getAppRenderKey"
          class="apps-list"
          :animation="300"
          handle=".drag-handle-list"
          :delay="180"
          :delay-on-touch-only="true"
          :touch-start-threshold="8"
          :fallback-tolerance="8"
          :disabled="false"
          ghost-class="app-list-item-ghost"
          chosen-class="app-list-item-chosen"
          drag-class="app-list-item-drag"
          @start="onDragStart"
          @end="onDragEnd"
        >
          <template #item="{ element: app, index }">
            <div class="app-list-wrapper" :class="{ 'selection-mode': selectionMode, 'is-selected': isAppSelected(index) }">
              <div
                v-if="selectionMode"
                class="app-select-checkbox app-select-checkbox--list"
                role="checkbox"
                tabindex="0"
                :aria-checked="isAppSelected(index)"
                :aria-label="$t('apps.batch_select_toggle')"
                @click.stop="toggleAppSelection(index)"
                @keydown.space.prevent="toggleAppSelection(index)"
                @keydown.enter.prevent="toggleAppSelection(index)"
              >
                <i class="fas" :class="isAppSelected(index) ? 'fa-square-check' : 'fa-square'"></i>
              </div>
              <AppListItem
                :app="app"
                :draggable="!selectionMode"
                :is-dragging="isDragging"
                @edit="selectionMode ? toggleAppSelection(index) : editApp(index)"
                @delete="showDeleteForm(index)"
                @copy-success="handleCopySuccess"
                @copy-error="handleCopyError"
              />
            </div>
          </template>
        </draggable>

        <!-- 列表视图 - 搜索模式 -->
        <div v-else-if="viewMode === 'list' && searchQuery" class="apps-list">
          <div
            v-for="({ app, index: originalIndex }, index) in filteredAppsWithIndex"
            :key="`search-list-${app.name}-${originalIndex}-${index}`"
            class="app-list-wrapper"
            :class="{ 'selection-mode': selectionMode, 'is-selected': isAppSelected(originalIndex) }"
          >
            <div
              v-if="selectionMode"
              class="app-select-checkbox app-select-checkbox--list"
              role="checkbox"
              tabindex="0"
              :aria-checked="isAppSelected(originalIndex)"
              :aria-label="$t('apps.batch_select_toggle')"
              @click.stop="toggleAppSelection(originalIndex)"
              @keydown.space.prevent="toggleAppSelection(originalIndex)"
              @keydown.enter.prevent="toggleAppSelection(originalIndex)"
            >
              <i class="fas" :class="isAppSelected(originalIndex) ? 'fa-square-check' : 'fa-square'"></i>
            </div>
            <AppListItem
              :app="app"
              :draggable="false"
              :is-search-result="true"
              :is-dragging="false"
              @edit="selectionMode ? toggleAppSelection(originalIndex) : editApp(originalIndex)"
              @delete="showDeleteForm(originalIndex)"
              @copy-success="handleCopySuccess"
              @copy-error="handleCopyError"
            />
          </div>
        </div>

        <!-- 空状态 - 搜索无结果 -->
        <div v-if="searchQuery && filteredApps.length === 0" class="empty-state">
          <div class="empty-icon">
            <i class="fas fa-search"></i>
          </div>
          <h3 class="empty-title">未找到匹配的应用</h3>
          <p class="empty-subtitle">尝试使用不同的搜索关键词</p>
        </div>

        <!-- 空状态 - 无应用 -->
        <div v-if="!searchQuery && apps.length === 0 && isLoaded" class="empty-state">
          <div class="empty-icon">
            <i class="fas fa-rocket"></i>
          </div>
          <h3 class="empty-title">暂无应用</h3>
          <p class="empty-subtitle">点击下方按钮添加第一个应用</p>
          <button class="btn btn-primary" @click="newApp">
            <i class="fas fa-plus me-1"></i>{{ $t('apps.add_new') }}
          </button>
        </div>
      </div>

      <!-- 应用编辑器 -->
      <AppEditor
        v-if="editingApp"
        :app="editingApp"
        :platform="platform"
        :disabled="isSaving"
        @save-app="handleSaveApp"
        @close="closeAppEditor"
      />

      <!-- 提示消息 -->
      <div v-if="message" class="alert-toast" :class="messageClass">
        <i class="fas" :class="getMessageIcon()"></i>
        <span>{{ message }}</span>
        <button class="btn-close-toast" @click="message = ''">
          <i class="fas fa-times"></i>
        </button>
      </div>

      <!-- 扫描结果模态框 -->
      <ScanResultModal
        :show="showScanResult"
        :apps="scannedApps"
        :saving="isSaving"
        :enhancement-progress="scanProgress"
        :enhancement-progress-percent="scanProgressPercent"
        @close="closeScanResult"
        @edit="handleScanEdit"
        @quick-add="quickAddScannedApp"
        @remove="removeScannedApp"
        @add-all="addAllScannedApps"
      />

      <Transition name="fade">
        <div v-if="showScanOptions" class="scan-options-overlay" @click.self="closeScanOptions">
          <div class="scan-options-modal">
            <div class="scan-options-header">
              <h5>
                <i class="fas fa-gamepad me-2"></i>扫描游戏资源
              </h5>
              <button class="btn-close" type="button" aria-label="Close" @click="closeScanOptions"></button>
            </div>

            <div class="scan-options-body">
              <section class="scan-options-section">
                <div class="scan-options-title">扫描范围</div>
                <label class="scan-option-row">
                  <input v-model="scanOptions.scope" type="radio" value="libraries" />
                  <span>
                    <strong>游戏平台库</strong>
                    <small>扫描已安装的 Steam、Epic Games 和 GOG 游戏</small>
                  </span>
                </label>
                <label class="scan-option-row">
                  <input v-model="scanOptions.scope" type="radio" value="directory" />
                  <span>
                    <strong>自选目录</strong>
                    <small>选择一个本地目录，扫描其中的可启动程序</small>
                  </span>
                </label>
              </section>

              <section v-if="scanOptions.scope === 'libraries'" class="scan-options-section">
                <div class="scan-options-title">游戏平台</div>
                <div class="scan-platform-grid">
                  <label v-for="platformOption in scanPlatformOptions" :key="platformOption.id" class="scan-pill-toggle">
                    <input v-model="scanOptions.platforms[platformOption.id]" type="checkbox" />
                    <span>{{ platformOption.label }}</span>
                  </label>
                </div>
              </section>

              <section v-if="scanOptions.scope === 'directory'" class="scan-options-section">
                <div class="scan-options-title">目录扫描</div>
                <label class="scan-option-row scan-option-row--compact">
                  <input v-model="scanOptions.extractIcons" type="checkbox" />
                  <span>
                    <strong>提取应用图标</strong>
                    <small>扫描速度会稍慢，但结果更容易辨认</small>
                  </span>
                </label>
              </section>

              <section class="scan-options-section">
                <div class="scan-options-title">AI 增强</div>
                <div class="scan-enhancement-list">
                  <label
                    v-for="skill in selectableGameLibrarySkills"
                    :key="skill.skillId"
                    class="scan-option-row scan-option-row--compact"
                  >
                    <input
                      type="checkbox"
                      :checked="isGameLibrarySkillEnabled(skill.skillId)"
                      @change="toggleGameLibrarySkill(skill.skillId)"
                    />
                    <span>
                      <strong>{{ getGameLibrarySkillLabel(skill.skillId) }}</strong>
                      <small>作为扫描后的增强步骤执行</small>
                    </span>
                    <i class="fas scan-option-icon" :class="getGameLibrarySkillIcon(skill.skillId)"></i>
                  </label>
                </div>
              </section>
            </div>

            <div class="scan-options-footer">
              <button class="btn btn-secondary" type="button" :disabled="isScanning" @click="closeScanOptions">
                取消
              </button>
              <button class="btn btn-primary" type="button" :disabled="isScanning" @click="runConfiguredScan">
                <i class="fas me-1" :class="isScanning ? 'fa-spinner fa-spin' : 'fa-search'"></i>
                开始扫描
              </button>
            </div>
          </div>
        </div>
      </Transition>

      <!-- 环境变量说明模态框 -->
      <div id="envVarsModal" class="modal fade" tabindex="-1" aria-labelledby="envVarsModalLabel">
        <div class="modal-dialog modal-lg env-vars-modal">
          <div class="modal-content">
            <div class="modal-header">
              <h5 id="envVarsModalLabel" class="modal-title">
                <i class="fas fa-info-circle me-2"></i>{{ $t('apps.env_vars_about') }}
              </h5>
              <button
                type="button"
                class="btn-close"
                data-bs-dismiss="modal"
                :aria-label="$t('_common.close') || '关闭'"
              ></button>
            </div>
            <div class="modal-body">
              <div class="alert alert-info">
                <div class="form-text">
                  <h6>{{ $t('apps.env_vars_about') }}</h6>
                  {{ $t('apps.env_vars_desc') }}
                </div>
              </div>
              <div class="env-vars-table">
                <div class="table-responsive">
                  <table class="table table-sm">
                    <thead>
                      <tr>
                        <th>
                          <i class="fas fa-code me-1"></i>{{ $t('apps.env_var_name') }}
                        </th>
                        <th>
                          <i class="fas fa-info-circle me-1"></i>{{ $t('_common.description') }}
                        </th>
                      </tr>
                    </thead>
                    <tbody>
                      <tr v-for="(desc, varName) in envVars" :key="varName">
                        <td>
                          <code class="env-var-name">{{ varName }}</code>
                        </td>
                        <td>{{ desc }}</td>
                      </tr>
                    </tbody>
                  </table>
                </div>
              </div>
              <div class="mt-3">
                <template v-if="platform === 'windows'">
                  <div class="form-text">
                    <strong>{{ $t('apps.env_qres_example') }}</strong>
                    <pre class="code-example">
cmd /C &lt;{{
                        $t('apps.env_qres_path')
                      }}&gt;\QRes.exe /X:%SUNSHINE_CLIENT_WIDTH% /Y:%SUNSHINE_CLIENT_HEIGHT% /R:%SUNSHINE_CLIENT_FPS%</pre
                    >
                  </div>
                </template>
                <template v-else-if="platform === 'linux'">
                  <div class="form-text">
                    <strong>{{ $t('apps.env_xrandr_example') }}</strong>
                    <pre class="code-example">
sh -c "xrandr --output HDMI-1 --mode \"${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT}\" --rate ${SUNSHINE_CLIENT_FPS}"</pre
                    >
                  </div>
                </template>
                <template v-else-if="platform === 'macos'">
                  <div class="form-text">
                    <strong>{{ $t('apps.env_displayplacer_example') }}</strong>
                    <pre class="code-example">
sh -c "displayplacer "id:&lt;screenId&gt; res:${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT} hz:${SUNSHINE_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre
                    >
                  </div>
                </template>
              </div>
            </div>
            <div class="modal-footer">
              <a
                href="https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2app__examples.html"
                target="_blank"
                rel="noopener noreferrer"
                class="btn btn-outline-primary"
              >
                <i class="fas fa-external-link-alt me-1"></i>{{ $t('_common.see_more') }}
              </a>
              <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">
                <i class="fas fa-times me-1"></i>{{ $t('_common.close') || '关闭' }}
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- 删除确认对话框 -->
    <Transition name="fade">
      <div v-if="deleteConfirmIndex !== null" class="delete-app-overlay" @click.self="cancelDeleteApp">
        <div class="delete-app-modal" role="dialog" aria-modal="true" aria-labelledby="deleteAppTitle">
          <div class="delete-app-header">
            <h5 id="deleteAppTitle">
              <i class="fas fa-exclamation-triangle me-2"></i>{{ $t('_common.delete') }}
            </h5>
            <button
              type="button"
              class="btn-close delete-app-close"
              :aria-label="$t('_common.close') || '关闭'"
              @click="cancelDeleteApp"
            ></button>
          </div>
          <div class="delete-app-body">
            <p>{{ $t('apps.delete_confirm', { name: apps[deleteConfirmIndex]?.name || '' }) }}</p>
          </div>
          <div class="delete-app-footer">
            <button type="button" class="btn btn-secondary" @click="cancelDeleteApp">{{ $t('_common.cancel') }}</button>
            <button type="button" class="btn btn-danger" @click="confirmDeleteApp">
              {{ $t('_common.delete') }}
            </button>
          </div>
        </div>
      </div>
    </Transition>
    <!-- 批量删除确认对话框 -->
    <Transition name="fade">
      <div v-if="batchDeleteConfirm" class="delete-app-overlay" @click.self="cancelBatchDelete">
        <div class="delete-app-modal" role="dialog" aria-modal="true" aria-labelledby="batchDeleteAppTitle">
          <div class="delete-app-header">
            <h5 id="batchDeleteAppTitle">
              <i class="fas fa-exclamation-triangle me-2"></i>{{ $t('apps.batch_delete') }}
            </h5>
            <button
              type="button"
              class="btn-close delete-app-close"
              :aria-label="$t('_common.close') || '关闭'"
              @click="cancelBatchDelete"
            ></button>
          </div>
          <div class="delete-app-body">
            <p>{{ $t('apps.batch_delete_confirm', { count: selectedIndices.size }) }}</p>
          </div>
          <div class="delete-app-footer">
            <button type="button" class="btn btn-secondary" :disabled="isBatchDeleting" @click="cancelBatchDelete">
              {{ $t('_common.cancel') }}
            </button>
            <button type="button" class="btn btn-danger" :disabled="isBatchDeleting" @click="confirmBatchDelete">
              <i v-if="isBatchDeleting" class="fas fa-spinner fa-spin me-1"></i>
              {{ $t('_common.delete') }}
            </button>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>

<script setup>
import { defineAsyncComponent, ref, onMounted, watch } from 'vue'
import draggable from 'vuedraggable-es'
import Navbar from '../components/layout/Navbar.vue'
import AppCard from '../components/AppCard.vue'
import AppListItem from '../components/AppListItem.vue'
import ScanResultModal from '../components/ScanResultModal.vue'
import { useApps } from '../composables/useApps.js'
import { initFirebase, trackEvents } from '../config/firebase.js'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()

const AppEditor = defineAsyncComponent(() => import('../components/AppEditor.vue'))

const isLoaded = ref(false)

const {
  apps,
  filteredApps,
  filteredAppsWithIndex,
  searchQuery,
  editingApp,
  platform,
  isSaving,
  isDragging,
  viewMode,
  message,
  envVars,
  debouncedSearch,
  messageClass,
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
  isAppSelected,
  selectAllFiltered,
  clearSelection,
  askBatchDelete,
  cancelBatchDelete,
  confirmBatchDelete,
  save,
  hasUnsavedChanges,
  restoreDefaultApps,
  onDragStart,
  onDragEnd,
  openScanOptions,
  closeScanOptions,
  runConfiguredScan,
  addScannedApp,
  addAllScannedApps,
  closeScanResult,
  removeScannedApp,
  quickAddScannedApp,
  isTauriEnv,
  isGameLibrarySkillEnabled,
  toggleGameLibrarySkill,
  getGameLibrarySkillIcon,
  getGameLibrarySkillLabel,
  getMessageIcon,
  handleCopySuccess,
  handleCopyError,
  init,
} = useApps()

const initEnvVarsModal = () => {
  try {
    const modalElement = document.getElementById('envVarsModal')
    if (modalElement && window.bootstrap?.Modal) {
      new window.bootstrap.Modal(modalElement)
    }
  } catch (error) {
    console.warn('Environment variables modal initialization failed:', error)
  }
}

onMounted(async () => {
  initFirebase()
  trackEvents.pageView('applications')
  init(t)
  initEnvVarsModal()

  await Promise.all([loadApps(), loadPlatform()])
  isLoaded.value = true
})

watch(searchQuery, () => {
  debouncedSearch.value?.()
})

// 处理扫描结果编辑
const handleScanEdit = (app) => {
  addScannedApp(app)
  closeScanResult()
}
</script>

<style>
@import '../styles/apps.less';
</style>
