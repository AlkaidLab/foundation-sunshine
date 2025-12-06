<template>
  <div>
    <Navbar />
    <div class="container py-4">
      <div class="row justify-content-center">
        <div class="col-lg-8 col-xl-7">
          <div class="text-center mb-4">
            <div class="icon-wrapper mb-2">
              <Icon name="lock" :size="32" icon-class="text-primary" />
            </div>
            <h1 class="h4 page-title">{{ $t('password.password_change') }}</h1>
            <p class="text-muted small mb-0">{{ $t('password.new_username_desc') }}</p>
          </div>

          <form @submit.prevent="save">
            <div class="card border-0 shadow-sm rounded-3">
              <div class="card-body p-3 p-md-4">
                <div class="row g-3">
                  <!-- Current Credentials Section -->
                  <div class="col-12">
                    <div class="section-header d-flex align-items-center mb-3">
                      <div class="section-icon me-2">
                        <Icon name="clock" :size="20" icon-class="text-primary" />
                      </div>
                      <h6 class="mb-0 fw-semibold">{{ $t('password.current_creds') }}</h6>
                    </div>
                    <div class="row g-2">
                      <div class="col-md-6">
                        <label for="currentUsername" class="form-label fw-medium small">
                          <i class="bi bi-person me-1 text-muted"></i>
                          {{ $t('_common.username') }}
                        </label>
                        <input
                          required
                          type="text"
                          class="form-control"
                          id="currentUsername"
                          v-model="passwordData.currentUsername"
                          :placeholder="$t('_common.username')"
                        />
                      </div>
                      <div class="col-md-6">
                        <label for="currentPassword" class="form-label fw-medium small">
                          <i class="bi bi-key me-1 text-muted"></i>
                          {{ $t('_common.password') }}
                        </label>
                        <input
                          autocomplete="current-password"
                          type="password"
                          class="form-control"
                          id="currentPassword"
                          v-model="passwordData.currentPassword"
                          :placeholder="$t('_common.password')"
                        />
                      </div>
                    </div>
                  </div>

                  <div class="col-12">
                    <hr class="my-1" />
                  </div>

                  <!-- New Credentials Section -->
                  <div class="col-12">
                    <div class="section-header d-flex align-items-center mb-3">
                      <div class="section-icon section-icon-success me-2">
                        <Icon name="star" :size="20" icon-class="text-success" />
                      </div>
                      <h6 class="mb-0 fw-semibold">{{ $t('password.new_creds') }}</h6>
                    </div>
                    <div class="row g-2">
                      <div class="col-12">
                        <label for="newUsername" class="form-label fw-medium small">
                          <i class="bi bi-person me-1 text-muted"></i>
                          {{ $t('_common.username') }}
                          <span class="text-muted fw-normal ms-1">({{ $t('_common.auto') }})</span>
                        </label>
                        <input
                          type="text"
                          class="form-control"
                          id="newUsername"
                          v-model="passwordData.newUsername"
                          :placeholder="$t('_common.username')"
                        />
                      </div>
                      <div class="col-md-6">
                        <label for="newPassword" class="form-label fw-medium small">
                          <i class="bi bi-lock me-1 text-muted"></i>
                          {{ $t('_common.password') }}
                        </label>
                        <input
                          autocomplete="new-password"
                          required
                          type="password"
                          class="form-control"
                          id="newPassword"
                          v-model="passwordData.newPassword"
                          :placeholder="$t('_common.password')"
                        />
                      </div>
                      <div class="col-md-6">
                        <label for="confirmNewPassword" class="form-label fw-medium small">
                          <i class="bi bi-lock-fill me-1 text-muted"></i>
                          {{ $t('password.confirm_password') }}
                        </label>
                        <input
                          autocomplete="new-password"
                          required
                          type="password"
                          class="form-control"
                          id="confirmNewPassword"
                          v-model="passwordData.confirmNewPassword"
                          :placeholder="$t('password.confirm_password')"
                        />
                      </div>
                    </div>
                  </div>
                </div>

                <!-- Alerts -->
                <div class="alert alert-danger d-flex align-items-center mt-3 rounded-3 py-2" v-if="error">
                  <i class="bi bi-exclamation-triangle-fill me-2"></i>
                  <div class="small">
                    <strong>{{ $t('_common.error') }}</strong> {{ error }}
                  </div>
                </div>
                <div class="alert alert-success d-flex align-items-center mt-3 rounded-3 py-2" v-if="success">
                  <i class="bi bi-check-circle-fill me-2"></i>
                  <div class="small">
                    <strong>{{ $t('_common.success') }}</strong> {{ $t('password.success_msg') }}
                  </div>
                </div>

                <!-- Submit Button -->
                <div class="d-grid mt-3">
                  <button class="btn btn-primary py-2 rounded-3" type="submit">
                    <i class="bi bi-save me-2"></i>{{ $t('_common.save') }}
                  </button>
                </div>
              </div>
            </div>
          </form>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import Icon from '../components/common/Icon.vue'

const error = ref(null)
const success = ref(false)
const passwordData = ref({
  currentUsername: '',
  currentPassword: '',
  newUsername: '',
  newPassword: '',
  confirmNewPassword: '',
})

async function save() {
  error.value = null
  try {
    const response = await fetch('/api/password', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(passwordData.value),
    })

    const result = await response.json()
    if (response.ok && result.status?.toString() === 'true') {
      success.value = true
      setTimeout(() => document.location.reload(), 5000)
    } else {
      error.value = result.error || 'Internal Server Error'
    }
  } catch (err) {
    error.value = err.message || 'Internal Server Error'
  }
}
</script>

<style>
@import '../styles/global.less';
</style>

<style lang="less" scoped>
@transition-duration: 0.2s;
@transition-timing: ease;
@primary-bg-opacity: 0.1;
@success-bg-opacity: 0.1;

.icon-wrapper {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 56px;
  height: 56px;
  background: rgba(var(--bs-primary-rgb), @primary-bg-opacity);
  border-radius: 50%;
}

.section-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  background: rgba(var(--bs-primary-rgb), @primary-bg-opacity);
  border-radius: 8px;
  font-size: 1rem;

  &-success {
    background: rgba(var(--bs-success-rgb), @success-bg-opacity);
  }
}

.form-control {
  padding: 0.5rem 0.75rem;
  border-radius: 0.375rem;
  transition: border-color @transition-duration @transition-timing,
              box-shadow @transition-duration @transition-timing;

  &:focus {
    box-shadow: 0 0 0 0.15rem rgba(var(--bs-primary-rgb), 0.15);
  }
}

.card {
  transition: transform @transition-duration @transition-timing;
}

.btn-primary {
  transition: transform @transition-duration @transition-timing,
              box-shadow @transition-duration @transition-timing;

  &:hover {
    transform: translateY(-1px);
    box-shadow: 0 4px 12px rgba(var(--bs-primary-rgb), 0.35);
  }
}

hr {
  opacity: 0.1;
}
</style>
