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
              :title="'网格视图'"
            >
              <i class="fas fa-th"></i>
            </button>
            <button
              class="view-toggle-btn"
              :class="{ active: viewMode === 'list' }"
              @click="viewMode = 'list'"
              :title="'列表视图'"
            >
              <i class="fas fa-list"></i>
            </button>
          </div>

          <button class="cute-btn cute-btn-primary" @click="newApp" :title="$t('apps.add_new')">
            <i class="fas fa-plus"></i>
          </button>
          <button
            class="cute-btn cute-btn-secondary"
            data-bs-toggle="modal"
            data-bs-target="#envVarsModal"
            :title="'环境变量说明'"
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
              :key="'search-' + index"
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
              :key="'search-' + index"
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

        <!-- 空状态 -->
        <div
          v-if="(searchQuery && filteredApps.length === 0) || (!searchQuery && apps.length === 0)"
          class="empty-state"
        >
          <div class="empty-icon">
            <i class="fas fa-rocket"></i>
          </div>
          <h3 class="empty-title">
            {{ searchQuery ? '未找到匹配的应用' : '暂无应用' }}
          </h3>
          <p class="empty-subtitle">
            {{ searchQuery ? '尝试使用不同的搜索关键词' : '点击下方按钮添加第一个应用' }}
          </p>
          <button v-if="!searchQuery" class="btn btn-primary" @click="newApp">
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

      <!-- 环境变量说明模态框 -->
      <div class="modal fade" id="envVarsModal" tabindex="-1">
        <div class="modal-dialog modal-lg">
          <div class="modal-content">
            <div class="modal-header">
              <h5 class="modal-title" id="envVarsModalLabel">
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
                <div class="form-text" v-if="platform === 'windows'">
                  <strong>{{ $t('apps.env_qres_example') }}</strong>
                  <pre class="code-example">
cmd /C &lt;{{
                      $t('apps.env_qres_path')
                    }}&gt;\QRes.exe /X:%SUNSHINE_CLIENT_WIDTH% /Y:%SUNSHINE_CLIENT_HEIGHT% /R:%SUNSHINE_CLIENT_FPS%</pre
                  >
                </div>
                <div class="form-text" v-else-if="platform === 'linux'">
                  <strong>{{ $t('apps.env_xrandr_example') }}</strong>
                  <pre class="code-example">
sh -c "xrandr --output HDMI-1 --mode \"${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT}\" --rate ${SUNSHINE_CLIENT_FPS}"</pre
                  >
                </div>
                <div class="form-text" v-else-if="platform === 'macos'">
                  <strong>{{ $t('apps.env_displayplacer_example') }}</strong>
                  <pre class="code-example">
sh -c "displayplacer "id:&lt;screenId&gt; res:${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT} hz:${SUNSHINE_CLIENT_FPS} scaling:on origin:(0,0) degree:0""</pre
                  >
                </div>
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
import { onMounted, watch } from 'vue'
import draggable from 'vuedraggable-es'
import Navbar from '../components/layout/Navbar.vue'
import AppEditor from '../components/AppEditor.vue'
import AppCard from '../components/AppCard.vue'
import AppListItem from '../components/AppListItem.vue'
import { useApps } from '../composables/useApps.js'
import { initFirebase, trackEvents } from '../config/firebase.js'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()

// 使用组合式函数
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
  messageType,
  envVars,
  debouncedSearch,
  messageClass,
  loadApps,
  loadPlatform,
  performSearch,
  clearSearch,
  getOriginalIndex,
  newApp,
  editApp,
  closeAppEditor,
  handleSaveApp,
  showDeleteForm,
  deleteApp,
  save,
  onDragStart,
  onDragEnd,
  showMessage,
  getMessageIcon,
  handleCopySuccess,
  handleCopyError,
  init,
} = useApps()

// 初始化
onMounted(async () => {
  // 初始化Firebase Analytics
  initFirebase()

  // 记录页面访问
  trackEvents.pageView('applications')

  // 初始化组合式函数
  init(t)

  // 初始化环境变量模态框
  try {
    const modalElement = document.getElementById('envVarsModal')
    if (modalElement && window.bootstrap?.Modal) {
      // 使用全局 Bootstrap 对象（已在 init.js 中导入并设置到 window.bootstrap）
      new window.bootstrap.Modal(modalElement)
    }
  } catch (error) {
    console.warn('Environment variables modal initialization failed:', error)
  }

  // 加载数据
  await loadApps()
  await loadPlatform()
})

// 监听搜索查询变化
watch(searchQuery, () => {
  if (debouncedSearch.value) {
    debouncedSearch.value()
  }
})
</script>

<style scoped>
@import '../styles/apps.css';
</style>
