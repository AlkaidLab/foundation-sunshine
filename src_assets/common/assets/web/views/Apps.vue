<template>
  <div>
    <Navbar />
    <div class="container-fluid px-4">
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
            @input="debouncedSearch"
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
            v-if="isTauriEnv()"
            class="cute-btn cute-btn-info"
            @click="scanDirectory(true)"
            :disabled="isScanning"
            title="扫描目录添加应用"
          >
            <i class="fas" :class="isScanning ? 'fa-spinner fa-spin' : 'fa-folder-open'"></i>
          </button>
          <button
            class="cute-btn cute-btn-secondary"
            data-bs-toggle="modal"
            data-bs-target="#envVarsModal"
            title="环境变量说明"
          >
            <i class="fas fa-info-circle"></i>
          </button>
          <button class="cute-btn cute-btn-success" @click="save" :title="$t('_common.save')">
            <i class="fas fa-save"></i>
          </button>
        </div>
      </div>

      <!-- 应用卡片列表 -->
      <div class="apps-grid-container">
        <!-- 网格视图 -->
        <template v-if="viewMode === 'grid'">
          <draggable
            v-if="!searchQuery"
            v-model="apps"
            item-key="name"
            class="apps-grid"
            :animation="300"
            :delay="0"
            :disabled="false"
            ghost-class="app-card-ghost"
            chosen-class="app-card-chosen"
            drag-class="app-card-drag"
            @start="onDragStart"
            @end="onDragEnd"
          >
            <template #item="{ element: app, index }">
              <AppCard
                :app="app"
                :draggable="true"
                :is-drag-result="false"
                :is-dragging="isDragging"
                @edit="editApp(index)"
                @delete="showDeleteForm(index)"
                @copy-success="handleCopySuccess"
                @copy-error="handleCopyError"
              />
            </template>
          </draggable>

          <div v-else class="apps-grid">
            <AppCard
              v-for="(app, index) in filteredApps"
              :key="`search-${index}`"
              :app="app"
              :draggable="false"
              :is-search-result="true"
              :is-dragging="false"
              @edit="editApp(getOriginalIndex(app, index))"
              @delete="showDeleteForm(getOriginalIndex(app, index))"
              @copy-success="handleCopySuccess"
              @copy-error="handleCopyError"
            />
          </div>
        </template>

        <!-- 列表视图 -->
        <template v-else>
          <draggable
            v-if="!searchQuery"
            v-model="apps"
            item-key="name"
            class="apps-list"
            :animation="300"
            :delay="0"
            :disabled="false"
            ghost-class="app-list-item-ghost"
            chosen-class="app-list-item-chosen"
            drag-class="app-list-item-drag"
            @start="onDragStart"
            @end="onDragEnd"
          >
            <template #item="{ element: app, index }">
              <AppListItem
                :app="app"
                :draggable="true"
                :is-dragging="isDragging"
                @edit="editApp(index)"
                @delete="showDeleteForm(index)"
                @copy-success="handleCopySuccess"
                @copy-error="handleCopyError"
              />
            </template>
          </draggable>

          <div v-else class="apps-list">
            <AppListItem
              v-for="(app, index) in filteredApps"
              :key="`search-${index}`"
              :app="app"
              :draggable="false"
              :is-search-result="true"
              :is-dragging="false"
              @edit="editApp(getOriginalIndex(app, index))"
              @delete="showDeleteForm(getOriginalIndex(app, index))"
              @copy-success="handleCopySuccess"
              @copy-error="handleCopyError"
            />
          </div>
        </template>

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
      <Transition name="fade">
        <div v-if="showScanResult" class="scan-result-overlay" @click.self="closeScanResult">
          <div class="scan-result-modal">
            <div class="scan-result-header">
              <h5>
                <i class="fas fa-search me-2"></i>扫描结果
                <span class="badge bg-primary ms-2">{{ scannedApps.length }}</span>
                <span v-if="scannedAppsSearchQuery" class="badge bg-info ms-2">
                  匹配: {{ filteredScannedApps.length }}
                </span>
              </h5>
              <button class="btn-close" @click="closeScanResult"></button>
            </div>
            <!-- 搜索框 -->
            <div v-if="scannedApps.length > 0" class="scan-result-search">
              <div class="search-box">
                <i class="fas fa-search search-icon"></i>
                <input
                  type="text"
                  class="form-control search-input"
                  placeholder="搜索应用名称、命令或路径..."
                  v-model="scannedAppsSearchQuery"
                />
                <button
                  v-if="scannedAppsSearchQuery"
                  class="btn-clear-search"
                  @click="scannedAppsSearchQuery = ''"
                  type="button"
                >
                  <i class="fas fa-times"></i>
                </button>
              </div>
            </div>
            <div class="scan-result-body">
              <div v-if="scannedApps.length === 0" class="text-center text-muted py-4">
                <i class="fas fa-folder-open fa-3x mb-3"></i>
                <p>未找到可添加的应用程序</p>
              </div>
              <div v-else-if="filteredScannedApps.length === 0" class="text-center text-muted py-4">
                <i class="fas fa-search fa-3x mb-3"></i>
                <p>未找到匹配的应用</p>
                <p class="small">尝试使用不同的搜索关键词</p>
              </div>
              <div v-else class="scan-result-list">
                <div v-for="(app, index) in filteredScannedApps" :key="app.source_path" class="scan-result-item">
                  <div class="scan-app-icon">
                    <img
                      v-if="app['image-path']"
                      :src="app['image-path']"
                      :alt="app.name"
                      @error="$event.target.style.display = 'none'"
                    />
                    <svg v-else width="100" height="100" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
                      <rect width="100" height="100" fill="#667eea"/>
                      <text x="50" y="50" font-size="40" font-weight="bold" fill="#fff" text-anchor="middle" dominant-baseline="central">{{ app.name.charAt(0).toUpperCase() }}</text>
                    </svg>
                  </div>
                  <div class="scan-app-info">
                    <div class="scan-app-name">{{ app.name }}</div>
                    <div class="scan-app-cmd small">{{ app.cmd }}</div>
                    <div class="scan-app-path small"><i class="fas fa-folder-open me-1"></i>{{ app.source_path }}</div>
                  </div>
                  <div class="scan-app-actions">
                    <button
                      class="btn btn-sm btn-outline-primary"
                      @click="addScannedApp(app), closeScanResult()"
                      title="添加并编辑"
                    >
                      <i class="fas fa-edit"></i>
                    </button>
                    <button
                      class="btn btn-sm btn-outline-success"
                      @click="quickAddScannedApp(app, scannedApps.indexOf(app))"
                      title="直接添加"
                    >
                      <i class="fas fa-plus"></i>
                    </button>
                    <button
                      class="btn btn-sm btn-outline-danger"
                      @click="removeScannedApp(scannedApps.indexOf(app))"
                      title="从列表移除"
                    >
                      <i class="fas fa-times"></i>
                    </button>
                  </div>
                </div>
              </div>
            </div>
            <div v-if="scannedApps.length > 0" class="scan-result-footer">
              <button class="btn btn-secondary" @click="closeScanResult"><i class="fas fa-times me-1"></i>关闭</button>
              <button class="btn btn-primary" @click="addAllScannedApps" :disabled="isSaving">
                <i class="fas" :class="isSaving ? 'fa-spinner fa-spin' : 'fa-check-double'"></i>
                <span class="ms-1">全部添加</span>
              </button>
            </div>
          </div>
        </div>
      </Transition>

      <!-- 环境变量说明模态框 -->
      <div id="envVarsModal" class="modal fade" tabindex="-1">
        <div class="modal-dialog modal-lg">
          <div class="modal-content">
            <div class="modal-header">
              <h5 id="envVarsModalLabel" class="modal-title">
                <i class="fas fa-info-circle me-2"></i>{{ $t('apps.env_vars_about') }}
              </h5>
              <button type="button" class="btn-close" data-bs-dismiss="modal" aria-label="Close"></button>
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
                  <table class="table">
                    <thead>
                      <tr>
                        <th>
                          <strong>{{ $t('apps.env_var_name') }}</strong>
                        </th>
                        <th><strong>说明</strong></th>
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
                href="https://docs.lizardbyte.dev/projects/sunshine/en/latest/about/guides/app_examples.html"
                target="_blank"
                class="btn btn-outline-primary"
              >
                <i class="fas fa-external-link-alt me-1"></i>{{ $t('_common.see_more') }}
              </a>
              <button type="button" class="btn btn-secondary" data-bs-dismiss="modal">
                <i class="fas fa-times me-1"></i>关闭
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, watch } from 'vue'
import draggable from 'vuedraggable-es'
import Navbar from '../components/layout/Navbar.vue'
import AppEditor from '../components/AppEditor.vue'
import AppCard from '../components/AppCard.vue'
import AppListItem from '../components/AppListItem.vue'
import { useApps } from '../composables/useApps.js'
import { initFirebase, trackEvents } from '../config/firebase.js'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()

const isLoaded = ref(false)

const {
  apps,
  filteredApps,
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
  scannedApps,
  showScanResult,
  scannedAppsSearchQuery,
  filteredScannedApps,
  loadApps,
  loadPlatform,
  clearSearch,
  getOriginalIndex,
  newApp,
  editApp,
  closeAppEditor,
  handleSaveApp,
  showDeleteForm,
  save,
  onDragStart,
  onDragEnd,
  scanDirectory,
  addScannedApp,
  addAllScannedApps,
  closeScanResult,
  removeScannedApp,
  quickAddScannedApp,
  isTauriEnv,
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
</script>

<style>
@import '../styles/apps.less';
</style>
