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
            <p class="mb-3 text-muted">{{ $t('pin.remove_paired_devices_desc') }}</p>
          </div>

          <!-- 加载状态 -->
          <div v-if="loading && clients.length === 0" class="text-center py-5">
            <div class="spinner-border text-primary" role="status">
              <span class="visually-hidden">{{ $t('pin.loading') }}</span>
            </div>
            <p class="mt-3 text-muted">{{ $t('pin.loading_clients') }}</p>
          </div>

          <!-- 客户端列表 -->
          <div id="client-list" v-else-if="clients && clients.length > 0" class="client-list-container">
            <div class="table-responsive">
              <table class="table table-hover table-bordered align-middle mb-0">
                <thead class="table-dark">
                  <tr>
                    <th scope="col" width="20%" class="ps-3">{{ $t('pin.client_name') }}</th>
                    <th scope="col" class="ps-3">
                      <span class="d-inline-flex align-items-center gap-1">
                        {{ $t('pin.hdr_profile') }}
                        <i
                          class="fas fa-info-circle text-info"
                          data-tooltip="hdr-profile"
                          style="cursor: help; font-size: 0.875rem;"
                        ></i>
                      </span>
                    </th>
                    <th scope="col" class="ps-3">
                      <span class="d-inline-flex align-items-center gap-1">
                        {{ $t('pin.device_size') }}
                        <i
                          class="fas fa-info-circle text-info"
                          data-tooltip="device-size"
                          style="cursor: help; font-size: 0.875rem;"
                        ></i>
                      </span>
                    </th>
                    <th scope="col" width="30%" class="text-center">{{ $t('pin.actions') }}</th>
                  </tr>
                </thead>
                <tbody>
                  <tr
                    v-for="client in clients"
                    :key="client.uuid"
                    :class="{ 'table-warning': editingStates[client.uuid] }"
                  >
                    <td class="fw-medium ps-3">{{ client.name || $t('pin.unknown_client') }}</td>
                    <td class="ps-3">
                      <select
                        class="form-select form-select-sm"
                        v-model="client.hdrProfile"
                        :disabled="!editingStates[client.uuid]"
                        @change="onProfileChange(client.uuid)"
                      >
                        <option v-if="!hasIccFileList" value="" disabled>{{ $t('pin.modify_in_gui') }}</option>
                        <option v-else value="">{{ $t('pin.none') }}</option>
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
                        <option value="small">{{ $t('pin.device_size_small') }}</option>
                        <option value="medium">{{ $t('pin.device_size_medium') }}</option>
                        <option value="large">{{ $t('pin.device_size_large') }}</option>
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
                            :title="$t('pin.edit_client_settings')"
                          >
                            <i class="fas fa-edit me-1"></i> {{ $t('_common.edit') }}
                          </button>
                        </template>
                        <!-- 保存/取消按钮 -->
                        <template v-else>
                          <button
                            class="btn btn-sm btn-success me-1"
                            @click="handleSave(client.uuid)"
                            :disabled="saving || deleting.has(client.uuid)"
                            :title="$t('pin.save_changes')"
                          >
                            <span v-if="saving" class="spinner-border spinner-border-sm me-1"></span>
                            <i v-else class="fas fa-check me-1"></i> {{ $t('_common.save') }}
                          </button>
                          <button
                            class="btn btn-sm btn-secondary me-1"
                            @click="handleCancelEdit(client.uuid)"
                            :disabled="saving || deleting.has(client.uuid)"
                            :title="$t('pin.cancel_editing')"
                          >
                            <i class="fas fa-times me-1"></i> {{ $t('_common.cancel') }}
                          </button>
                        </template>
                        <!-- 删除按钮 -->
                        <button
                          class="btn btn-sm btn-outline-danger"
                          @click="handleDelete(client)"
                          :disabled="saving || deleting.has(client.uuid) || editingStates[client.uuid]"
                          :title="editingStates[client.uuid] ? $t('pin.save_or_cancel_first') : $t('pin.delete_client')"
                        >
                          <span v-if="deleting.has(client.uuid)" class="spinner-border spinner-border-sm me-1"></span>
                          <i v-else class="fas fa-trash me-1"></i> {{ $t('_common.delete') }}
                        </button>
                      </div>
                      <!-- 未保存更改提示 -->
                      <div
                        v-if="editingStates[client.uuid] && hasUnsavedChanges(client.uuid)"
                        class="text-warning small mt-2"
                      >
                        <i class="fas fa-exclamation-triangle me-1"></i> {{ $t('pin.unsaved_changes') }}
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
      <Transition name="fade">
        <div v-if="clientToDelete" class="delete-client-overlay" @click.self="clientToDelete = null">
          <div class="delete-client-modal">
            <div class="delete-client-header">
              <h5>
                <i class="fas fa-exclamation-triangle me-2"></i>{{ $t('pin.confirm_delete') }}
              </h5>
              <button class="btn-close" @click="clientToDelete = null"></button>
            </div>
            <div class="delete-client-body">
              <p v-html="$t('pin.delete_confirm_message', { name: clientToDelete.name || $t('pin.unknown_client') })"></p>
              <p class="text-muted small mb-0">{{ $t('pin.delete_warning') }}</p>
            </div>
            <div class="delete-client-footer">
              <button type="button" class="btn btn-secondary" @click="clientToDelete = null">{{ $t('_common.cancel') }}</button>
              <button type="button" class="btn btn-danger" @click="confirmDelete">
                <span v-if="deleting.has(clientToDelete.uuid)" class="spinner-border spinner-border-sm me-2"></span>
                {{ $t('_common.delete') }}
              </button>
            </div>
          </div>
        </div>
      </Transition>
    </div>
  </div>
</template>

<script setup>
import { onMounted, ref, nextTick, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { Tooltip } from 'bootstrap'
import Navbar from '../components/layout/Navbar.vue'
import { usePin } from '../composables/usePin.js'

const { t } = useI18n()

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

const handleDelete = (client) => {
  if (editingStates[client.uuid]) return
  clientToDelete.value = client
}

const confirmDelete = async () => {
  if (!clientToDelete.value) return
  const success = await unpairSingle(clientToDelete.value.uuid)
  if (success) clientToDelete.value = null
}

const handleSave = async (uuid) => {
  const success = await saveClient(uuid)
  if (!success) alert(t('pin.save_failed'))
}

const handleCancelEdit = (uuid) => cancelEdit(uuid)

const handleUnpairAll = async () => {
  if (confirm(t('pin.unpair_all_confirm'))) await unpairAll()
}

const initTooltips = () => {
  nextTick(() => {
    const tooltipConfigs = [
      { selector: '[data-tooltip="hdr-profile"]', title: t('pin.hdr_profile_info') },
      { selector: '[data-tooltip="device-size"]', title: t('pin.device_size_info') }
    ]
    
    tooltipConfigs.forEach(({ selector, title }) => {
      const el = document.querySelector(selector)
      if (!el) return
      
      Tooltip.getInstance(el)?.dispose()
      new Tooltip(el, { html: true, placement: 'top', title })
    })
  })
}

onMounted(async () => {
  await loadConfig()
  await refreshClients()

  initPinForm(() => setTimeout(refreshClients, 0))

  if (window.electron?.getIccFileList) {
    hasIccFileList.value = true
    window.electron.getIccFileList((files = []) => {
      hdrProfileList.value = files.filter(file => /.icc$/.test(file))
    })
  } else {
    hasIccFileList.value = false
  }

  initTooltips()
})

watch(clients, initTooltips, { deep: true })
</script>

<style>
@import '../styles/global.less';
</style>

<style scoped lang="less">
.client-list-container {
  margin-top: 1rem;

  .table-responsive {
    border-radius: var(--border-radius-md, 8px);
    overflow: hidden;
  }

  .table {
    border-radius: var(--border-radius-md, 12px);
    overflow: hidden;
    margin-bottom: 0;
  }
}

.table-warning {
  background-color: rgba(255, 193, 7, 0.1) !important;
}

/* Delete Client Modal - 使用 ScanResultModal 样式 */
.delete-client-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  width: 100vw;
  height: 100vh;
  margin: 0;
  background: var(--overlay-bg, rgba(0, 0, 0, 0.7));
  backdrop-filter: blur(8px);
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--spacing-lg, 20px);
  overflow: hidden;

  [data-bs-theme='light'] & {
    background: rgba(0, 0, 0, 0.5);
  }
}

.delete-client-modal {
  background: var(--modal-bg, rgba(30, 30, 50, 0.95));
  border: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.2));
  border-radius: var(--border-radius-xl, 12px);
  width: 100%;
  max-width: 500px;
  max-height: 80vh;
  display: flex;
  flex-direction: column;
  backdrop-filter: blur(20px);
  box-shadow: var(--shadow-xl, 0 25px 50px rgba(0, 0, 0, 0.5));
  animation: modalSlideUp 0.3s ease;

  [data-bs-theme='light'] & {
    background: rgba(255, 255, 255, 0.95);
    border: 1px solid rgba(0, 0, 0, 0.15);
    box-shadow: 0 25px 50px rgba(0, 0, 0, 0.2);
  }
}

@keyframes modalSlideUp {
  from {
    transform: translateY(20px);
    opacity: 0;
  }
  to {
    transform: translateY(0);
    opacity: 1;
  }
}

.delete-client-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-bottom: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  h5 {
    margin: 0;
    color: var(--text-primary, #fff);
    font-size: var(--font-size-lg, 1.1rem);
    font-weight: 600;
    display: flex;
    align-items: center;
    gap: var(--spacing-sm, 8px);
  }

  [data-bs-theme='light'] & {
    border-bottom: 1px solid rgba(0, 0, 0, 0.1);

    h5 {
      color: #000000;
    }
  }
}

.delete-client-body {
  padding: var(--spacing-lg, 24px);
  font-size: var(--font-size-md, 0.95rem);
  line-height: 1.5;
  overflow-y: auto;
  flex: 1;
  color: var(--text-primary, #fff);

  [data-bs-theme='light'] & {
    color: #000000;
  }
}

.delete-client-footer {
  display: flex;
  justify-content: flex-end;
  gap: 10px;
  padding: var(--spacing-md, 20px) var(--spacing-lg, 24px);
  border-top: 1px solid var(--border-color-light, rgba(255, 255, 255, 0.1));

  [data-bs-theme='light'] & {
    border-top: 1px solid rgba(0, 0, 0, 0.1);
  }

  button {
    padding: 8px 16px;
    font-size: 0.9rem;
  }
}

/* Vue 过渡动画 */
.fade-enter-active,
.fade-leave-active {
  transition: opacity 0.3s ease;
}

.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

/* 响应式优化 */
@media (max-width: 768px) {
  .btn-toolbar {
    flex-direction: column;

    .btn {
      width: 100%;
      margin-bottom: 0.25rem;
    }
  }

  .table-responsive {
    font-size: 0.875rem;
  }
}
</style>
