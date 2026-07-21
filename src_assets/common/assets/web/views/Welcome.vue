<template>
  <div id="content" class="container welcome-page">
    <div class="row justify-content-center my-4">
      <div class="col-lg-10">
        <!-- 语言选择器 -->
        <div class="text-end mb-3">
          <div class="language-selector">
            <label for="localeSelect" class="form-label me-2">
              <i class="fas fa-language"></i>
            </label>
            <select
              id="localeSelect"
              class="form-select form-select-sm d-inline-block"
              style="width: auto;"
              v-model="selectedLocale"
              @change="changeLanguage"
            >
              <option value="en">English</option>
              <option value="en_GB">English (UK)</option>
              <option value="en_US">English (US)</option>
              <option value="zh">简体中文</option>
              <option value="zh_TW">繁體中文</option>
              <option value="de">Deutsch</option>
              <option value="fr">Français</option>
              <option value="es">Español</option>
              <option value="it">Italiano</option>
              <option value="ja">日本語</option>
              <option value="ko">한국어</option>
              <option value="ru">Русский</option>
              <option value="uk">Українська</option>
              <option value="pt">Português</option>
              <option value="pt_BR">Português (Brasil)</option>
              <option value="pl">Polski</option>
              <option value="sv">Svenska</option>
              <option value="tr">Türkçe</option>
              <option value="cs">Čeština</option>
              <option value="bg">Български</option>
            </select>
          </div>
        </div>

        <div class="card my-4">
          <div class="card-body">
            <header class="text-center mb-4">
              <h1 class="mb-3">
                {{ $t('welcome.greeting') }}
              </h1>
              <p class="lead text-muted">{{ $t('welcome.create_creds') }}</p>
            </header>

            <div class="alert alert-warning">
              <i class="fas fa-exclamation-triangle me-2"></i>
              {{ $t('welcome.create_creds_alert') }}
              <br>
              <i class="fas fa-shield-alt me-2 mt-2"></i>
              {{ $t('welcome.creds_local_only') }}
            </div>

            <form @submit.prevent="save">
              <div class="row justify-content-center">
                <div class="col-md-8">
                  <div class="mb-3">
                    <label for="usernameInput" class="form-label">
                      <i class="fas fa-user me-2"></i>{{ $t('welcome.username') }}
                    </label>
                    <input
                      type="text"
                      class="form-control"
                      id="usernameInput"
                      autocomplete="username"
                      v-model="passwordData.newUsername"
                      :placeholder="$t('welcome.username')"
                    />
                  </div>

                  <div class="mb-3">
                    <label for="passwordInput" class="form-label">
                      <i class="fas fa-lock me-2"></i>{{ $t('welcome.password') }}
                    </label>
                    <div class="input-group">
                      <input
                        :type="showPassword ? 'text' : 'password'"
                        class="form-control"
                        id="passwordInput"
                        autocomplete="new-password"
                        v-model="passwordData.newPassword"
                        :placeholder="$t('welcome.password')"
                        required
                      />
                      <button class="btn btn-outline-secondary toggle-password" type="button" @click="showPassword = !showPassword" :aria-label="showPassword ? $t('welcome.hide_password') : $t('welcome.show_password')">
                        <i :class="showPassword ? 'fas fa-eye-slash' : 'fas fa-eye'"></i>
                      </button>
                    </div>
                  </div>

                  <div class="mb-3">
                    <label for="confirmPasswordInput" class="form-label">
                      <i class="fas fa-check-circle me-2"></i>{{ $t('welcome.confirm_password') }}
                    </label>
                    <div class="input-group" :class="{ 'is-invalid': !passwordsMatch && passwordData.confirmNewPassword }">
                      <input
                        :type="showConfirmPassword ? 'text' : 'password'"
                        class="form-control"
                        id="confirmPasswordInput"
                        autocomplete="new-password"
                        v-model="passwordData.confirmNewPassword"
                        :placeholder="$t('welcome.confirm_password')"
                        required
                      />
                      <button class="btn btn-outline-secondary toggle-password" type="button" @click="showConfirmPassword = !showConfirmPassword" :aria-label="showConfirmPassword ? $t('welcome.hide_password') : $t('welcome.show_password')">
                        <i :class="showConfirmPassword ? 'fas fa-eye-slash' : 'fas fa-eye'"></i>
                      </button>
                    </div>
                    <div class="invalid-feedback d-block" v-if="!passwordsMatch && passwordData.confirmNewPassword">
                      <i class="fas fa-exclamation-circle me-1"></i>{{ $t('welcome.password_mismatch') }}
                    </div>
                    <div
                      class="valid-feedback d-block"
                      v-if="passwordsMatch && passwordData.confirmNewPassword && passwordData.newPassword"
                    >
                      <i class="fas fa-check-circle me-1"></i>{{ $t('welcome.password_match') }}
                    </div>
                  </div>

                  <button type="submit" class="btn btn-primary w-100 mb-3" :disabled="loading || !isFormValid">
                    <span
                      v-if="loading"
                      class="spinner-border spinner-border-sm me-2"
                      role="status"
                      aria-hidden="true"
                    ></span>
                    <i v-else class="fas fa-sign-in-alt me-2"></i>
                    {{ $t('welcome.login') }}
                  </button>

                  <transition name="fade">
                    <div class="alert alert-danger" v-if="error">
                      <i class="fas fa-exclamation-circle me-2"></i>
                      <strong>{{ $t('welcome.error') }}</strong>
                      <span v-if="error.startsWith('welcome.')">{{ $t(error) }}</span>
                      <span v-else>{{ error }}</span>
                    </div>
                  </transition>

                  <transition name="fade">
                    <div class="alert alert-success" v-if="success">
                      <i class="fas fa-check-circle me-2"></i>
                      <strong>{{ $t('welcome.success') }}</strong> {{ $t('welcome.welcome_success') }}
                    </div>
                  </transition>
                </div>
              </div>
            </form>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useWelcome } from '../composables/useWelcome.js'
import { getBootstrapLocale } from '../config/bootstrapData.js'
import { loadAutoTheme } from '../utils/theme.js'

const { locale, setLocaleMessage } = useI18n()
const selectedLocale = ref('en')

const { error, success, loading, passwordData, passwordsMatch, isFormValid, save } = useWelcome()
const showPassword = ref(false)
const showConfirmPassword = ref(false)

loadAutoTheme()

// 加载语言（使用静态嵌入的翻译数据）
const loadLanguage = (lang) => {
  const welcomeLocales = window.__WELCOME_LOCALES__
  
  if (welcomeLocales && welcomeLocales[lang]) {
    // 使用嵌入的翻译数据设置到 i18n
    setLocaleMessage(lang, welcomeLocales[lang])
    locale.value = lang
    document.querySelector('html').setAttribute('lang', lang)
  } else {
    // 如果没有找到，使用英文
    locale.value = 'en'
    document.querySelector('html').setAttribute('lang', 'en')
  }
}

// 加载当前语言设置
onMounted(async () => {
  try {
    // 使用 /api/configLocale，这个 API 不需要认证（在 welcome 页面时可能还没有账号）
    const config = await getBootstrapLocale()
    if (config.locale) {
      selectedLocale.value = config.locale
      loadLanguage(config.locale)
    } else {
      loadLanguage('en')
    }
  } catch (e) {
    console.error('Failed to load locale config', e)
    loadLanguage('en')
  }
})

// 切换语言
const changeLanguage = () => {
  loadLanguage(selectedLocale.value)
}
</script>

<style scoped>  
.welcome-page {
  --sketch-black: var(--ui-text-primary);
  --sketch-blue: var(--ui-accent);
  --sketch-green: var(--ui-success);
  --sketch-red: var(--ui-danger);
  --sketch-yellow: var(--ui-warning);
  --paper-bg: var(--ui-surface-strong);
  --pencil-gray: var(--ui-text-secondary);
}

/* 容器定位 */
#content {
  position: relative;
  z-index: 1;
  padding-top: 1rem;
  padding-bottom: 2rem;
}

/* 主认证卡片 */
.card {
  background: var(--ui-surface-strong);
  border: 1px solid var(--ui-border);
  border-radius: var(--ui-radius-lg);
  box-shadow: var(--ui-shadow-md);
  position: relative;
  transform: none;
  filter: none;
  backdrop-filter: blur(20px);
  overflow: hidden;
}

.card-body {
  padding: clamp(1.25rem, 4vw, 2.5rem);
}

/* 旧装饰层已停用 */
.card::before {
  display: none;
}

/* 旧装饰层已停用 */
.card::after {
  display: none;
}

/* 页面标题 */
h1 {
  font-family: inherit;
  color: var(--sketch-black);
  font-weight: 700;
  text-shadow: none;
  transform: none;
  position: relative;
}

/* 标题下划线效果 */
h1::after {
  content: '';
  position: absolute;
  bottom: -0.55rem;
  left: 34%;
  right: 34%;
  height: 2px;
  background: var(--sketch-blue);
  border-radius: 999px;
  opacity: 0.75;
}

/* Logo 效果 */
header img {
  filter: drop-shadow(3px 3px 0px rgba(0, 0, 0, 0.1));
}

/* 副标题 */
.lead {
  font-family: inherit;
  color: var(--pencil-gray);
  font-size: 1.1rem;
  transform: none;
}

/* 安全提示 */
.alert-warning {
  background: color-mix(in srgb, var(--ui-warning) 12%, var(--ui-surface-strong));
  border: 1px solid color-mix(in srgb, var(--ui-warning) 34%, transparent);
  border-radius: var(--ui-radius-md);
  box-shadow: none;
  transform: none;
  position: relative;
  font-family: inherit;
  color: var(--ui-warning-text);
  font-size: 0.95rem;
  font-weight: 500;
  padding: 1rem 1.25rem;
}

.alert-warning i {
  color: var(--ui-warning);
  filter: none;
}

/* 表单标签 */
.form-label {
  font-family: inherit;
  color: var(--sketch-black);
  font-size: 0.95rem;
  font-weight: 600;
  transform: none;
  display: inline-block;
  position: relative;
}

/* 标签强调下划线 */
.form-label::after {
  content: '';
  position: absolute;
  bottom: 0;
  left: 0;
  width: 100%;
  height: 2px;
  background: var(--sketch-blue);
  opacity: 0.3;
  transform: scaleX(0);
  transition: transform 0.3s ease;
}

.form-label:hover::after {
  transform: scaleX(1);
}

.form-label i {
  color: var(--sketch-blue);
  margin-right: 0.3rem;
}

.toggle-password {
  border: 1px solid var(--ui-border) !important;
  border-left: 0 !important;
  border-radius: 0 8px 8px 0 !important;
  background: var(--ui-surface) !important;
  color: var(--ui-text-secondary) !important;
  padding: 0 14px;
  transition: all 0.2s ease;
  outline: none !important;
  box-shadow: none !important;
}

.toggle-password:hover,
.toggle-password:focus,
.toggle-password:active {
  background: var(--ui-accent-soft) !important;
  color: var(--ui-accent) !important;
  border-color: var(--ui-border-strong) !important;
  border-left: none !important;
  outline: none !important;
  box-shadow: none !important;
}

.input-group .form-control {
  border-radius: 8px 0 0 8px;
  border-right: none;
}

/* 输入控件 */
.form-control {
  background: var(--ui-surface);
  border: 1px solid var(--ui-border);
  border-radius: 8px;
  padding: 12px 16px;
  font-family: inherit;
  font-size: 1rem;
  color: var(--sketch-black);
  box-shadow: none;
  transition: all 0.3s ease;
  position: relative;
}

.form-control:focus {
  outline: none;
  border-color: var(--sketch-blue);
  background: var(--ui-surface-hover);
  box-shadow: 0 0 0 0.2rem var(--ui-accent-soft);
  transform: none;
}

.form-control::placeholder {
  color: var(--pencil-gray);
  opacity: 0.6;
}

/* 验证状态 */
.form-control.is-invalid {
  border-color: var(--sketch-red);
  background: var(--ui-danger-soft);
  animation: shake 0.5s;
}

@keyframes shake {
  0%,
  100% {
    transform: translateX(0) rotate(0deg);
  }
  25% {
    transform: translateX(-5px) rotate(-1deg);
  }
  75% {
    transform: translateX(5px) rotate(1deg);
  }
}

.form-control:focus.is-invalid {
  border-color: var(--sketch-red);
  box-shadow: 0 0 0 0.2rem var(--ui-danger-soft);
}

/* 反馈信息 */
.invalid-feedback,
.valid-feedback {
  display: block;
  margin-top: 0.5rem;
  font-family: inherit;
  font-size: 0.9rem;
  font-weight: 600;
}

.invalid-feedback {
  color: var(--sketch-red);
}

.valid-feedback {
  color: var(--sketch-green);
}

.invalid-feedback i,
.valid-feedback i {
  display: inline-block;
}

/* 主要操作 */
.btn-primary {
  background: var(--sketch-blue);
  border: 1px solid var(--sketch-blue);
  border-radius: 10px;
  padding: 12px 28px;
  font-family: inherit;
  font-size: 1rem;
  font-weight: 600;
  color: var(--ui-accent-contrast);
  text-shadow: none;
  box-shadow: var(--ui-shadow-sm);
  transition: all 0.2s ease;
  position: relative;
  overflow: hidden;
}

/* 按钮装饰线条 */
.btn-primary::before {
  display: none;
}

.btn-primary:hover:not(:disabled) {
  transform: translateY(-1px);
  box-shadow: var(--ui-shadow-md);
}

.btn-primary:active:not(:disabled) {
  transform: translateY(0);
  box-shadow: var(--ui-shadow-sm);
}

.btn-primary:disabled {
  opacity: 0.5;
  cursor: not-allowed;
  background: var(--pencil-gray);
}

.btn-primary i {
  display: inline-block;
}

/* 加载动画 */
.spinner-border {
  border-color: rgba(255, 255, 255, 0.3);
  border-right-color: #fff;
}

/* 错误提示 */
.alert-danger {
  background: var(--ui-danger-soft);
  border: 1px solid var(--ui-danger-border);
  border-radius: 10px;
  color: var(--ui-danger-text);
  box-shadow: none;
  transform: none;
  animation: errorPop 0.5s ease;
  position: relative;
}

@keyframes errorPop {
  0% {
    transform: scale(0.8) rotate(0.5deg);
    opacity: 0;
  }
  50% {
    transform: scale(1.05) rotate(0.5deg);
  }
  100% {
    transform: scale(1) rotate(0.5deg);
    opacity: 1;
  }
}

.alert-danger::before {
  content: '❌';
  position: absolute;
  top: -12px;
  left: 10px;
  font-size: 1.5rem;
}

/* 成功提示 */
.alert-success {
  background: color-mix(in srgb, var(--ui-success) 12%, var(--ui-surface-strong));
  border: 1px solid color-mix(in srgb, var(--ui-success) 34%, transparent);
  border-radius: 10px;
  color: var(--ui-success-text);
  box-shadow: none;
  transform: none;
  animation: successPop 0.6s ease;
  position: relative;
}

@keyframes successPop {
  0% {
    transform: scale(0.8) rotate(-0.5deg);
    opacity: 0;
  }
  50% {
    transform: scale(1.05) rotate(-0.5deg);
  }
  100% {
    transform: scale(1) rotate(-0.5deg);
    opacity: 1;
  }
}

.alert-success::before {
  content: '✓';
  position: absolute;
  top: -12px;
  left: 10px;
  font-size: 1.8rem;
  font-weight: bold;
  color: var(--sketch-green);
}

.alert-danger::before,
.alert-success::before {
  display: none;
}

/* Vue 过渡动画 */
.fade-enter-active {
  animation: sketchIn 0.5s ease;
}

.fade-leave-active {
  animation: sketchOut 0.3s ease;
}

@keyframes sketchIn {
  0% {
    opacity: 0;
    transform: translateY(12px) scale(0.98);
  }
  100% {
    opacity: 1;
    transform: translateY(0) scale(1);
  }
}

@keyframes sketchOut {
  0% {
    opacity: 1;
    transform: translateY(0);
  }
  100% {
    opacity: 0;
    transform: translateY(-12px);
  }
}

/* SVG 滤镜 */
svg {
  position: absolute;
  width: 0;
  height: 0;
}

/* 响应式优化 */
@media (max-width: 768px) {
  .card {
    transform: rotate(0deg);
    margin: 1rem 0.5rem;
  }

  h1 {
    font-size: 1.5rem;
  }

  .btn-primary {
    font-size: 1rem;
    padding: 12px 24px;
  }

  .col-md-8 {
    padding-left: 0.5rem;
    padding-right: 0.5rem;
  }
}

/* 语言选择器样式 */
.language-selector {
  display: inline-block;
  position: relative;
}

.language-selector .form-label {
  font-family: inherit;
  color: var(--sketch-black);
  font-size: 1rem;
  font-weight: 600;
  margin-bottom: 0;
  vertical-align: middle;
}

.language-selector .form-select-sm {
  font-family: inherit;
  border: 1px solid var(--ui-border);
  border-radius: 8px;
  padding: 6px 12px;
  font-size: 0.95rem;
  color: var(--ui-text-primary);
  background: var(--ui-surface-strong);
  box-shadow: var(--ui-shadow-sm);
  transition: all 0.3s ease;
  cursor: pointer;
}

.language-selector .form-select-sm:focus {
  outline: none;
  border-color: var(--sketch-blue);
  background: var(--ui-surface-hover);
  box-shadow: 0 0 0 0.2rem var(--ui-accent-soft);
  transform: none;
}

.language-selector .form-select-sm:hover {
  transform: translateY(-1px);
  box-shadow: var(--ui-shadow-md);
}

/* 打印样式优化 */
@media print {
  .card::after,
  .alert-warning::before,
  .alert-danger::before,
  .alert-success::before,
  .language-selector {
    display: none;
  }
}
</style>
