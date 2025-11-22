<template>
  <div>
    <Navbar />
    <div id="content" class="container">
      <h1 class="my-4 text-center">{{ $t('pin.pin_pairing') }}</h1>
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
            <div class="d-flex justify-content-end align-items-center">
              <h2 id="unpair" class="text-center me-auto">{{ $t('troubleshooting.unpair_title') }}</h2>
              <button class="btn btn-danger" :disabled="unpairAllPressed" @click="unpairAll">
                {{ $t('troubleshooting.unpair_all') }}
              </button>
            </div>
            <div
              id="apply-alert"
              class="alert alert-success d-flex align-items-center mt-3"
              :style="{ 'display': (showApplyMessage ? 'flex !important': 'none !important') }"
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
            <br />
            <p class="mb-0">Remove your paired devices.</p>
          </div>

          <div
            id="client-list"
            class="list-group list-group-flush list-group-item-light"
            v-if="clients && clients.length > 0"
          >
            <table class="table">
              <thead>
                <tr>
                  <th scope="col" width="25%">Name</th>
                  <th scope="col">HDR Profile</th>
                  <th scope="col" width="25%">Actions</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="(client, i) in clients" :key="client.uuid">
                  <td style="vertical-align: middle">{{ client.name || "Unknown Client" }}</td>
                  <td>
                    <select class="form-select" v-model="client.hdrProfile" :disabled="saved">
                      <option v-for="item in hdrProfileList" :value="item" :key="item">{{ item }}</option>
                    </select>
                  </td>
                  <td>
                    <button v-show="saved" class="btn btn-primary mx-1" :disabled="!saved" @click="saved = !saved">
                      <i class="fas fa-edit"></i> Edit
                    </button>
                    <button v-show="!saved" class="btn btn-primary mx-1" :form="'form-client-' + i">Save</button>
                    <button class="btn btn-danger mx-1" @click="unpairSingle(client.uuid)">
                      <i class="fas fa-trash"></i> Delete
                    </button>
                    <form :id="'form-client-' + i" @submit.prevent="save()"></form>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
          <div v-else class="list-group list-group-flush list-group-item-light">
            <div class="list-group-item p-3 text-center">
              <em>{{ $t('troubleshooting.unpair_single_no_devices') }}</em>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import { usePin } from '../composables/usePin.js'

const {
  pairingDeviceName,
  unpairAllPressed,
  unpairAllStatus,
  showApplyMessage,
  clients,
  hdrProfileList,
  saved,
  refreshClients,
  unpairAll,
  unpairSingle,
  save,
  initPinForm,
  clickedApplyBanner,
  loadConfig,
} = usePin()

onMounted(async () => {
  await loadConfig()
  await refreshClients()
  
  initPinForm(() => {
    setTimeout(() => refreshClients(), 0)
  })
  
  // 获取 HDR Profile 列表（如果 Electron 可用）
  if (window.electron?.getIccFileList) {
    window.electron.getIccFileList((files = []) => {
      hdrProfileList.value = files.filter((file) => /.icc$/.test(file))
    })
  }
})
</script>

<style scoped>
.container {
  padding: 1rem;
}
</style>

