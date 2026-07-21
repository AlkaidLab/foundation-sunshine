<template>
  <div class="dropdown account-menu">
    <button
      id="navbar-account-menu"
      type="button"
      class="btn nav-utility-button dropdown-toggle"
      data-bs-toggle="dropdown"
      aria-expanded="false"
      :aria-label="`${$t('navbar.password')} / ${$t('navbar.troubleshoot')}`"
      :title="`${$t('navbar.password')} / ${$t('navbar.troubleshoot')}`"
    >
      <i class="fas fa-user-circle" aria-hidden="true"></i>
      <span class="visually-hidden">{{ $t('navbar.password') }} / {{ $t('navbar.troubleshoot') }}</span>
    </button>
    <ul class="dropdown-menu dropdown-menu-end" aria-labelledby="navbar-account-menu">
      <li>
        <a class="dropdown-item" href="/password">
          <i class="fas fa-key fa-fw me-2" aria-hidden="true"></i>
          {{ $t('navbar.password') }}
        </a>
      </li>
      <li>
        <a class="dropdown-item" href="/troubleshooting">
          <i class="fas fa-tools fa-fw me-2" aria-hidden="true"></i>
          {{ $t('navbar.troubleshoot') }}
        </a>
      </li>
      <li><hr class="dropdown-divider" /></li>
      <li>
        <button type="button" class="dropdown-item text-danger" @click="handleLogout">
          <i class="fas fa-sign-out-alt fa-fw me-2" aria-hidden="true"></i>
          {{ $t('troubleshooting.logout') }}
        </button>
      </li>
    </ul>

    <ConfirmDialog
      :show="showLogoutDialog"
      dialog-id="account-logout-confirm"
      :title="$t('troubleshooting.confirm_logout')"
      title-icon="fas fa-sign-out-alt"
      tone="danger"
      :close-label="$t('_common.close')"
      @close="closeLogoutDialog"
    >
      <p>{{ $t('troubleshooting.confirm_logout_desc') }}</p>
      <template #actions>
        <button type="button" class="btn btn-secondary" @click="closeLogoutDialog">
          {{ $t('_common.cancel') }}
        </button>
        <button type="button" class="btn btn-danger" @click="confirmLogout">
          <i class="fas fa-sign-out-alt me-2" aria-hidden="true"></i>{{ $t('troubleshooting.logout') }}
        </button>
      </template>
    </ConfirmDialog>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import ConfirmDialog from './ConfirmDialog.vue'
import { useLogout } from '../../composables/useLogout.js'

const { logout } = useLogout()
const showLogoutDialog = ref(false)

const handleLogout = () => {
  showLogoutDialog.value = true
}

const closeLogoutDialog = () => {
  showLogoutDialog.value = false
}

const confirmLogout = () => {
  closeLogoutDialog()

  logout({
    onLocalhost: () => {
      window.location.href = '/'
    },
  })
}
</script>
