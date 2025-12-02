<template>
  <div id="content" class="container">
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="pencilTexture">
          <feTurbulence type="fractalNoise" baseFrequency="0.8" numOctaves="4" result="noise" />
          <feDisplacementMap in="SourceGraphic" in2="noise" scale="2" xChannelSelector="R" yChannelSelector="G" />
        </filter>
      </defs>
    </svg>

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
                <img src="/images/logo-sunshine-45.png" height="45" alt="" />
                {{ $t('welcome.greeting') }}
              </h1>
              <p class="lead text-muted">{{ $t('welcome.create_creds') }}</p>
            </header>

            <div class="alert alert-warning">
              <i class="fas fa-exclamation-triangle me-2"></i>
              {{ $t('welcome.create_creds_alert') }}
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
                    <input
                      type="password"
                      class="form-control"
                      id="passwordInput"
                      autocomplete="new-password"
                      v-model="passwordData.newPassword"
                      :placeholder="$t('welcome.password')"
                      required
                    />
                  </div>

                  <div class="mb-3">
                    <label for="confirmPasswordInput" class="form-label">
                      <i class="fas fa-check-circle me-2"></i>{{ $t('welcome.confirm_password') }}
                    </label>
                    <input
                      type="password"
                      class="form-control"
                      :class="{ 'is-invalid': !passwordsMatch && passwordData.confirmNewPassword }"
                      id="confirmPasswordInput"
                      autocomplete="new-password"
                      v-model="passwordData.confirmNewPassword"
                      :placeholder="$t('welcome.confirm_password')"
                      required
                    />
                    <div class="invalid-feedback" v-if="!passwordsMatch && passwordData.confirmNewPassword">
                      <i class="fas fa-exclamation-circle me-1"></i>密码不匹配
                    </div>
                    <div
                      class="valid-feedback"
                      v-if="passwordsMatch && passwordData.confirmNewPassword && passwordData.newPassword"
                    >
                      <i class="fas fa-check-circle me-1"></i>密码匹配
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

const { locale, setLocaleMessage } = useI18n()
const selectedLocale = ref('en')

const { error, success, loading, passwordData, passwordsMatch, isFormValid, save } = useWelcome()

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
    const config = await fetch('/api/configLocale').then((r) => r.json())
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
:root {
  --sketch-black: #2c2c2c;
  --sketch-blue: #4169e1;
  --sketch-green: #3cb371;
  --sketch-red: #dc143c;
  --sketch-yellow: #ffd700;
  --paper-bg: #fffef9;
  --pencil-gray: #8b8b8b;
}

/* 容器定位 */
#content {
  position: relative;
  z-index: 1;
}

/* 手绘风格卡片 */
.card {
  background: #fff;
  border: 3px solid var(--sketch-black);
  border-radius: 15px;
  box-shadow: 6px 6px 0px rgba(0, 0, 0, 0.1), 3px 3px 0px rgba(0, 0, 0, 0.05);
  position: relative;
  transform: rotate(-0.5deg);
  filter: url(#pencilTexture);
}

/* 手绘边框效果 */
.card::before {
  content: '';
  position: absolute;
  top: -3px;
  left: -3px;
  right: -3px;
  bottom: -3px;
  border: 2px dashed rgba(0, 0, 0, 0.1);
  border-radius: 15px;
  pointer-events: none;
}

/* 装饰性涂鸦 */
.card::after {
  content: '✨';
  position: absolute;
  top: -15px;
  right: 20px;
  font-size: 2rem;
  transform: rotate(15deg);
}

/* 手写标题 */
h1 {
  font-family: 'Patrick Hand', cursive;
  color: var(--sketch-black);
  font-weight: 700;
  text-shadow: 2px 2px 0px rgba(0, 0, 0, 0.05);
  transform: rotate(-1deg);
  position: relative;
}

/* 标题下划线效果 */
h1::after {
  content: '';
  position: absolute;
  bottom: -5px;
  left: 10%;
  right: 10%;
  height: 3px;
  background: var(--sketch-blue);
  border-radius: 50%;
  opacity: 0.3;
}

/* Logo 效果 */
header img {
  filter: drop-shadow(3px 3px 0px rgba(0, 0, 0, 0.1));
}

/* 副标题 */
.lead {
  font-family: 'Indie Flower', cursive;
  color: var(--pencil-gray);
  font-size: 1.1rem;
  transform: rotate(0.5deg);
}

/* 手绘警告框 */
.alert-warning {
  background: linear-gradient(135deg, #fff9e6 0%, #fffaeb 100%);
  border: 3px solid #ff8c00;
  border-radius: 12px;
  box-shadow: 5px 5px 0px rgba(255, 140, 0, 0.4), 3px 3px 0px rgba(255, 140, 0, 0.2), 0 0 20px rgba(255, 140, 0, 0.15);
  transform: rotate(-0.3deg);
  position: relative;
  font-family: 'Kalam', cursive;
  color: #d97706;
  font-size: 1.05rem;
  font-weight: 600;
  padding: 1rem 1.25rem;
}

.alert-warning i {
  color: var(--sketch-yellow);
  filter: drop-shadow(1px 1px 0px rgba(0, 0, 0, 0.2));
}

/* 手绘表单标签 */
.form-label {
  font-family: 'Patrick Hand', cursive;
  color: var(--sketch-black);
  font-size: 1.1rem;
  font-weight: 700;
  transform: rotate(-0.5deg);
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

/* 手绘输入框 */
.form-control {
  background: #fff;
  border: 2px solid var(--sketch-black);
  border-radius: 8px;
  padding: 12px 16px;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Microsoft YaHei', sans-serif;
  font-size: 1rem;
  color: var(--sketch-black);
  box-shadow: inset 2px 2px 0px rgba(0, 0, 0, 0.05), 2px 2px 0px rgba(0, 0, 0, 0.1);
  transition: all 0.3s ease;
  position: relative;
}

.form-control:focus {
  outline: none;
  border-color: var(--sketch-blue);
  background: #f0f8ff;
  box-shadow: inset 2px 2px 0px rgba(65, 105, 225, 0.1), 3px 3px 0px rgba(65, 105, 225, 0.2);
  transform: translateY(-2px);
}

.form-control::placeholder {
  color: var(--pencil-gray);
  opacity: 0.6;
}

/* 验证状态 */
.form-control.is-invalid {
  border-color: var(--sketch-red);
  background: #fff5f5;
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
  box-shadow: inset 2px 2px 0px rgba(220, 20, 60, 0.1), 3px 3px 0px rgba(220, 20, 60, 0.2);
}

/* 反馈信息 */
.invalid-feedback,
.valid-feedback {
  display: block;
  margin-top: 0.5rem;
  font-family: 'Kalam', cursive;
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

/* 手绘按钮 */
.btn-primary {
  background: var(--sketch-blue);
  border: 3px solid var(--sketch-black);
  border-radius: 10px;
  padding: 14px 32px;
  font-family: 'Patrick Hand', cursive;
  font-size: 1.2rem;
  font-weight: 700;
  color: #fff;
  text-shadow: 1px 1px 0px rgba(0, 0, 0, 0.2);
  box-shadow: 4px 4px 0px rgba(0, 0, 0, 0.2), inset -2px -2px 0px rgba(0, 0, 0, 0.1);
  transition: all 0.2s ease;
  position: relative;
  overflow: hidden;
}

/* 按钮装饰线条 */
.btn-primary::before {
  content: '';
  position: absolute;
  top: 3px;
  left: 3px;
  right: 3px;
  bottom: 3px;
  border: 1px dashed rgba(255, 255, 255, 0.3);
  border-radius: 7px;
  pointer-events: none;
}

.btn-primary:hover:not(:disabled) {
  transform: rotate(-0.5deg) translateY(-3px);
  box-shadow: 6px 6px 0px rgba(0, 0, 0, 0.25), inset -2px -2px 0px rgba(0, 0, 0, 0.1);
}

.btn-primary:active:not(:disabled) {
  transform: rotate(-0.5deg) translateY(0);
  box-shadow: 2px 2px 0px rgba(0, 0, 0, 0.2), inset -2px -2px 0px rgba(0, 0, 0, 0.1);
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

/* 手绘错误提示 */
.alert-danger {
  background: #fff0f0;
  border: 2px solid var(--sketch-red);
  border-radius: 10px;
  color: var(--sketch-red);
  box-shadow: 3px 3px 0px rgba(220, 20, 60, 0.2);
  transform: rotate(0.5deg);
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

/* 手绘成功提示 */
.alert-success {
  background: #f0fff4;
  border: 2px solid var(--sketch-green);
  border-radius: 10px;
  color: var(--sketch-green);
  box-shadow: 3px 3px 0px rgba(60, 179, 113, 0.2);
  transform: rotate(-0.5deg);
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
    transform: translateY(20px) rotate(-2deg) scale(0.95);
  }
  100% {
    opacity: 1;
    transform: translateY(0) rotate(0deg) scale(1);
  }
}

@keyframes sketchOut {
  0% {
    opacity: 1;
    transform: translateY(0) rotate(0deg);
  }
  100% {
    opacity: 0;
    transform: translateY(-20px) rotate(2deg);
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
  font-family: 'Patrick Hand', cursive;
  color: var(--sketch-black);
  font-size: 1rem;
  font-weight: 600;
  margin-bottom: 0;
  vertical-align: middle;
}

.language-selector .form-select-sm {
  font-family: 'Patrick Hand', cursive;
  border: 2px solid var(--sketch-black);
  border-radius: 8px;
  padding: 6px 12px;
  font-size: 0.95rem;
  color: var(--sketch-black);
  background: #fff;
  box-shadow: 2px 2px 0px rgba(0, 0, 0, 0.1);
  transition: all 0.3s ease;
  cursor: pointer;
}

.language-selector .form-select-sm:focus {
  outline: none;
  border-color: var(--sketch-blue);
  background: #f0f8ff;
  box-shadow: 3px 3px 0px rgba(65, 105, 225, 0.2);
  transform: translateY(-1px);
}

.language-selector .form-select-sm:hover {
  transform: translateY(-1px);
  box-shadow: 3px 3px 0px rgba(0, 0, 0, 0.15);
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
