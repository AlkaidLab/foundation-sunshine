<template>
  <div class="dropdown account-menu">
    <button
      id="navbar-account-menu"
      type="button"
      class="btn nav-utility-button dropdown-toggle"
      data-bs-toggle="dropdown"
      aria-expanded="false"
      :aria-label="$t('navbar.password')"
      :title="$t('navbar.password')"
    >
      <i class="fas fa-user-circle" aria-hidden="true"></i>
      <span class="visually-hidden">{{ $t('navbar.password') }}</span>
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
  </div>
</template>

<script setup>
import { useI18n } from 'vue-i18n'
import { useLogout } from '../../composables/useLogout.js'

const { t } = useI18n()
const { logout } = useLogout()

const handleLogout = () => {
  const confirmation = `${t('troubleshooting.confirm_logout')}\n\n${t('troubleshooting.confirm_logout_desc')}`
  if (typeof window.confirm === 'function' && !window.confirm(confirmation)) return

  logout({
    onLocalhost: () => {
      window.location.href = '/'
    },
  })
}
</script>
