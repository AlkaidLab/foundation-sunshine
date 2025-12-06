<template>
  <div>
    <Navbar />
    <div id="content" class="container">
      <h1 class="my-4 text-center page-title">{{ $t('pin.pin_pairing') }}</h1>
      <form action="" class="form d-flex flex-column align-items-center" id="form">
        <div class="card flex-column d-flex p-4 mb-4">
          <input
            type="text"
            pattern="\d*"
            :placeholder="`${$t('navbar.pin')}`"
            autofocus
            id="pin-input"
            class="form-control mt-2"
            required
          />
          <input
            type="text"
            v-model="pairingDeviceName"
            :placeholder="`${$t('pin.device_name')}`"
            id="name-input"
            class="form-control my-4"
            required
          />
          <button class="btn btn-primary">{{ $t('pin.send') }}</button>
        </div>
        <div class="alert alert-warning">
          <b>{{ $t('_common.warning') }}</b> {{ $t('pin.warning_msg') }}
        </div>
        <div id="status"></div>
      </form>

      <!-- Unpair all Clients -->
      <div class="card my-4">
        <div class="card-body">
          <div class="p-2">
            <div class="d-flex justify-content-end align-items-center mb-3">
              <h2 id="unpair" class="text-center me-auto mb-0">{{ $t('troubleshooting.unpair_title') }}</h2>
              <button class="btn btn-danger" :disabled="unpairAllPressed || loading" @click="handleUnpairAll">
                <span v-if="unpairAllPressed" class="spinner-border spinner-border-sm me-2" role="status"></span>
                {{ $t('troubleshooting.unpair_all') }}
              </button>
            </div>
            <div
              id="apply-alert"
              class="alert alert-success d-flex align-items-center mt-3"
              :style="{ display: showApplyMessage ? 'flex !important' : 'none !important' }"
            >
              <div class="me-2">
                <b>{{ $t('_common.success') }}</b> {{ $t('troubleshooting.unpair_single_success') }}
              </div>
              <button class="btn btn-success ms-auto apply" @click="clickedApplyBanner">
                {{ $t('_common.dismiss') }}
              </button>
            </div>
            <div class="alert alert-success" v-if="unpairAllStatus === true">
              {{ $t('troubleshooting.unpair_all_success') }}
            </div>
            <div class="alert alert-danger" v-if="unpairAllStatus === false">
              {{ $t('troubleshooting.unpair_all_error') }}
            </div>
            <p class="mb-3 text-muted">Remove your paired devices.</p>
          </div>

          <!-- 加载状态 -->
          <div v-if="loading && clients.length === 0" class="text-center py-5">
            <div class="spinner-border text-primary" role="status">
              <span class="visually-hidden">Loading...</span>
            </div>
            <p class="mt-3 text-muted">Loading clients...</p>
          </div>

          <!-- 客户端列表 -->
          <div id="client-list" v-else-if="clients && clients.length > 0" class="client-list-container">
            <div class="table-responsive">
              <table class="table table-hover table-bordered align-middle mb-0">
                <thead class="table-dark">
                  <tr>
                    <th scope="col" width="20%" class="ps-3">Name</th>
                    <th scope="col" class="ps-3">HDR Profile</th>
                    <th scope="col" class="ps-3">尺寸</th>
                    <th scope="col" width="30%" class="text-center">Actions</th>
                  </tr>
                </thead>
                <tbody>
                  <tr
                    v-for="client in clients"
                    :key="client.uuid"
                    :class="{ 'table-warning': editingStates[client.uuid] }"
                  >
                    <td class="fw-medium ps-3">{{ client.name || 'Unknown Client' }}</td>
                    <td class="ps-3">
                      <select
                        class="form-select form-select-sm"
                        v-model="client.hdrProfile"
                        :disabled="!editingStates[client.uuid]"
                        @change="onProfileChange(client.uuid)"
                      >
                        <option v-if="!hasIccFileList" value="" disabled>Please modify in GUI</option>
                        <option v-else value="">-- None --</option>
                        <option v-for="item in hdrProfileList" :value="item" :key="item">{{ item }}</option>
                      </select>
                    </td>
                    <td class="ps-3">
                      <select
                        class="form-select form-select-sm"
                        v-model="client.deviceSize"
                        :disabled="!editingStates[client.uuid]"
                        @change="onSizeChange(client.uuid)"
                      >
                        <option value="small">小 - 手机</option>
                        <option value="medium">中 - 平板</option>
                        <option value="large">大 - TV</option>
                      </select>
                    </td>
                    <td class="text-center">
                      <div class="btn-toolbar justify-content-center" role="toolbar">
                        <!-- 编辑模式按钮 -->
                        <template v-if="!editingStates[client.uuid]">
                          <button
                            class="btn btn-sm btn-outline-primary me-1"
                            @click="startEdit(client.uuid)"
                            :disabled="saving || deleting.has(client.uuid)"
                            title="Edit client settings"
                          >
                            <i class="fas fa-edit me-1"></i> Edit
                          </button>
                        </template>
                        <!-- 保存/取消按钮 -->
                        <template v-else>
                          <button
                            class="btn btn-sm btn-success me-1"
                            @click="handleSave(client.uuid)"
                            :disabled="saving || deleting.has(client.uuid)"
                            title="Save changes"
                          >
                            <span v-if="saving" class="spinner-border spinner-border-sm me-1"></span>
                            <i v-else class="fas fa-check me-1"></i> Save
                          </button>
                          <button
                            class="btn btn-sm btn-secondary me-1"
                            @click="handleCancelEdit(client.uuid)"
                            :disabled="saving || deleting.has(client.uuid)"
                            title="Cancel editing"
                          >
                            <i class="fas fa-times me-1"></i> Cancel
                          </button>
                        </template>
                        <!-- 删除按钮 -->
                        <button
                          class="btn btn-sm btn-outline-danger"
                          @click="handleDelete(client)"
                          :disabled="saving || deleting.has(client.uuid) || editingStates[client.uuid]"
                          :title="editingStates[client.uuid] ? 'Please save or cancel editing first' : 'Delete client'"
                        >
                          <span v-if="deleting.has(client.uuid)" class="spinner-border spinner-border-sm me-1"></span>
                          <i v-else class="fas fa-trash me-1"></i> Delete
                        </button>
                      </div>
                      <!-- 未保存更改提示 -->
                      <div
                        v-if="editingStates[client.uuid] && hasUnsavedChanges(client.uuid)"
                        class="text-warning small mt-2"
                      >
                        <i class="fas fa-exclamation-triangle me-1"></i> Unsaved changes
                      </div>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>
          <!-- 空状态 -->
          <div v-else-if="!loading" class="list-group list-group-flush list-group-item-light">
            <div class="list-group-item p-5 text-center">
              <i class="fas fa-inbox fa-3x text-muted mb-3"></i>
              <p class="mb-0">
                <em>{{ $t('troubleshooting.unpair_single_no_devices') }}</em>
              </p>
            </div>
          </div>
        </div>
      </div>

      <!-- 删除确认对话框 -->
      <div
        v-if="clientToDelete"
        class="modal fade show d-block"
        tabindex="-1"
        style="background-color: rgba(0, 0, 0, 0.5)"
        @click.self="clientToDelete = null"
      >
        <div class="modal-dialog modal-dialog-centered">
          <div class="modal-content">
            <div class="modal-header">
              <h5 class="modal-title">Confirm Delete</h5>
              <button type="button" class="btn-close" @click="clientToDelete = null"></button>
            </div>
            <div class="modal-body">
              <p>
                Are you sure you want to delete <strong>{{ clientToDelete.name || 'Unknown Client' }}</strong
                >?
              </p>
              <p class="text-muted small mb-0">This action cannot be undone.</p>
            </div>
            <div class="modal-footer">
              <button type="button" class="btn btn-secondary" @click="clientToDelete = null">Cancel</button>
              <button type="button" class="btn btn-danger" @click="confirmDelete">
                <span v-if="deleting.has(clientToDelete.uuid)" class="spinner-border spinner-border-sm me-2"></span>
                Delete
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, ref } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import { usePin } from '../composables/usePin.js'

const {
  pairingDeviceName,
  unpairAllPressed,
  unpairAllStatus,
  showApplyMessage,
  clients,
  hdrProfileList,
  hasIccFileList,
  loading,
  saving,
  deleting,
  editingStates,
  refreshClients,
  unpairAll,
  unpairSingle,
  saveClient,
  startEdit,
  cancelEdit,
  hasUnsavedChanges,
  initPinForm,
  clickedApplyBanner,
  loadConfig,
} = usePin()

const clientToDelete = ref(null)

// 处理删除
const handleDelete = (client) => {
  // 如果正在编辑，不允许删除
  if (editingStates[client.uuid]) {
    return
  }
  clientToDelete.value = client
}

// 确认删除
const confirmDelete = async () => {
  if (!clientToDelete.value) return
  const uuid = clientToDelete.value.uuid
  const success = await unpairSingle(uuid)
  if (success) {
    clientToDelete.value = null
  }
}

// 处理保存
const handleSave = async (uuid) => {
  const success = await saveClient(uuid)
  if (!success) {
    alert('Failed to save client settings. Please try again.')
  }
}

// 处理取消编辑
const handleCancelEdit = (uuid) => {
  cancelEdit(uuid)
}

// 处理HDR Profile变更
const onProfileChange = (uuid) => {
  // 可以在这里添加实时验证或其他逻辑
}

// 处理尺寸变更
const onSizeChange = (uuid) => {
  // 可以在这里添加实时验证或其他逻辑
}

// 处理取消所有配对
const handleUnpairAll = async () => {
  if (confirm('Are you sure you want to unpair all clients? This action cannot be undone.')) {
    await unpairAll()
  }
}

onMounted(async () => {
  await loadConfig()
  await refreshClients()

  initPinForm(() => {
    setTimeout(() => refreshClients(), 0)
  })

  // 获取 HDR Profile 列表（如果 Electron 可用）
  if (window.electron?.getIccFileList) {
    hasIccFileList.value = true
    window.electron.getIccFileList((files = []) => {
      hdrProfileList.value = files.filter((file) => /.icc$/.test(file))
    })
  } else {
    hasIccFileList.value = false
  }
})
</script>

<style>
@import '../styles/global.less';
</style>

<style scoped>
.client-list-container {
  margin-top: 1rem;
}

.table-warning {
  background-color: rgba(255, 193, 7, 0.1) !important;
}

.modal.show {
  display: block;
}

.modal.show .modal-dialog {
  margin-top: 15vh;
}

/* 响应式优化 */
@media (max-width: 768px) {
  .btn-toolbar {
    flex-direction: column;
  }

  .btn-toolbar .btn {
    width: 100%;
    margin-bottom: 0.25rem;
  }

  .table-responsive {
    font-size: 0.875rem;
  }
}
</style>
