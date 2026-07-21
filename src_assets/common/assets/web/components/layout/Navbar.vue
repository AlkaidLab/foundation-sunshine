<template>
  <nav
    class="navbar navbar-light navbar-expand-md navbar-background header"
    :class="{ 'navbar-embedded': isEmbeddedGui }"
  >
    <div class="container-fluid">
      <a class="navbar-brand brand-enhanced" href="/" title="Sunshine">
        <img src="/images/logo-sunshine-256.png" height="50" alt="Sunshine-Foundation" class="brand-logo" />
      </a>
      <button
        class="navbar-toggler"
        type="button"
        data-bs-toggle="collapse"
        data-bs-target="#navbarSupportedContent"
        aria-controls="navbarSupportedContent"
        aria-expanded="false"
        aria-label="Toggle navigation"
      >
        <span class="navbar-toggler-icon"></span>
      </button>
      <div class="collapse navbar-collapse" id="navbarSupportedContent">
        <ul class="navbar-nav me-auto mb-2 mb-md-0">
          <li v-for="item in navItems" :key="item.path" class="nav-item">
            <a
              class="nav-link"
              :class="{ active: isActive(item.path) }"
              :href="item.path"
              :aria-current="isActive(item.path) ? 'page' : undefined"
            >
              <i :class="['fas', 'fa-fw', item.icon]" aria-hidden="true"></i> {{ $t(item.label) }}
            </a>
          </li>
        </ul>
        <div class="navbar-utilities ms-md-2">
          <ThemeToggle />
          <AccountMenu />
        </div>
      </div>
    </div>
  </nav>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import ThemeToggle from '../common/ThemeToggle.vue'
import AccountMenu from '../common/AccountMenu.vue'
import { useBackground } from '../../composables/useBackground.js'

// 导航项配置
const navItems = Object.freeze([
  { path: '/', icon: 'fa-home', label: 'navbar.home' },
  { path: '/pin', icon: 'fa-qrcode', label: 'navbar.pin' },
  { path: '/apps', icon: 'fa-stream', label: 'navbar.applications' },
  { path: '/config', icon: 'fa-cog', label: 'navbar.configuration' },
])

// 使用背景管理 composable
const { loadBackground, addDragListeners } = useBackground()
const isEmbeddedGui = window.isTauri === true && window.parent !== window

if (isEmbeddedGui) {
  document.documentElement.dataset.sunshineGui = 'true'
}

// 当前路径（响应式）
const currentPath = ref(window.location.pathname)

// 检查路径是否激活
const isActive = (path) => {
  const current = currentPath.value
  if (path === '/') {
    return current === '/' || current === '/index.html'
  }
  const normalizedPath = path.replace(/\.html$/, '')
  return current === normalizedPath || current.startsWith(normalizedPath)
}

// 更新当前路径
const updateCurrentPath = () => {
  currentPath.value = window.location.pathname
}

// 清理函数引用
let removeDragListeners = null

// 链接点击处理函数
const handleLinkClick = (e) => {
  if (e.target.closest('a.nav-link')?.href) {
    setTimeout(updateCurrentPath, 0)
  }
}

// 错误处理函数
const handleBackgroundError = (error) => {
  console.error('Background error:', error)
}

onMounted(async () => {
  await loadBackground()
  updateCurrentPath()
  removeDragListeners = addDragListeners(handleBackgroundError)
  window.addEventListener('popstate', updateCurrentPath)
  document.addEventListener('click', handleLinkClick)
})

onUnmounted(() => {
  window.removeEventListener('popstate', updateCurrentPath)
  document.removeEventListener('click', handleLinkClick)
  removeDragListeners?.()
})
</script>

<style scoped>
.navbar-background {
  position: relative;
  z-index: 1030;
  background-color: var(--ui-nav-bg, rgba(240, 248, 255, 0.92));
  border-bottom: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  box-shadow: var(--ui-shadow-sm, 0 4px 16px rgba(74, 158, 255, 0.1));
  backdrop-filter: blur(14px);
}

.navbar-background.navbar-embedded {
  position: fixed;
  inset: 0 0 auto;
  z-index: 1030;
  margin-bottom: 0;
}

.navbar-embedded > .container-fluid {
  padding-right: 9rem;
}

.brand-enhanced {
  transition: transform 0.3s ease;
}

.brand-enhanced:hover {
  transform: scale(1.05) rotate(-5deg);
}

.navbar-utilities {
  display: flex;
  align-items: center;
  gap: 0.25rem;
}
</style>

<style>
html[data-sunshine-gui='true'] body {
  padding-top: 77px !important;
}

.header .navbar-utilities .nav-link,
.header .navbar-utilities .nav-utility-button {
  color: var(--ui-text-secondary, #64748b) !important;
  border: 0;
  border-radius: var(--ui-radius-sm, 8px);
  transition: color 0.2s ease, background-color 0.2s ease;
}

.header {
  view-transition-name: sunshine-navbar;
}

.header .navbar-utilities .nav-link:hover,
.header .navbar-utilities .nav-link:focus,
.header .navbar-utilities .nav-utility-button:hover,
.header .navbar-utilities .nav-utility-button:focus {
  color: var(--ui-accent, #4a9eff) !important;
  background-color: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
}

.header .navbar-utilities .nav-utility-button {
  padding: 0.5rem 0.75rem;
}

.header .navbar-utilities .dropdown-menu {
  min-width: 13rem;
  z-index: 1060;
}

.header .navbar-nav > .nav-item > .nav-link {
  position: relative;
  background: transparent;
  color: var(--ui-text-secondary, #64748b) !important;
  border-radius: 0;
  transition: color 0.2s ease;
}

.header .navbar-nav > .nav-item > .nav-link::after {
  position: absolute;
  right: 0.75rem;
  bottom: 0.3rem;
  left: 0.75rem;
  height: 2px;
  border-radius: 999px;
  background: var(--ui-accent, #4a9eff);
  content: '';
  transform: scaleX(0);
  transform-origin: center;
  transition: transform 0.2s ease;
}

.header .navbar-nav > .nav-item > .nav-link:hover,
.header .navbar-nav > .nav-item > .nav-link.active,
.header .navbar-nav > .nav-item > .nav-link:focus-visible {
  color: var(--ui-accent, #4a9eff) !important;
  background: transparent;
}

.header .navbar-nav > .nav-item > .nav-link.active::after,
.header .navbar-nav > .nav-item > .nav-link:focus-visible::after {
  transform: scaleX(1);
}

.header .navbar-nav > .nav-item > .nav-link:focus-visible {
  outline: none;
  box-shadow: none;
}

.header .navbar-nav > .nav-item > .nav-link:focus-visible::after {
  height: 3px;
}

.header .navbar-toggler {
  color: var(--ui-text-secondary, #64748b) !important;
  border: var(--bs-border-width) solid var(--ui-border, rgba(74, 158, 255, 0.22)) !important;
  background-color: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
}

.header .navbar-toggler-icon {
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%2833, 37, 41, 0.75%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e") !important;
}

[data-bs-theme='dark'] .header .navbar-toggler-icon {
  filter: invert(92%) sepia(13%) saturate(430%) hue-rotate(350deg) brightness(94%);
}

.form-control::placeholder {
  opacity: 0.5;
}

@media (max-width: 767.98px) {
  .header .navbar-collapse {
    flex-basis: 100%;
    padding: 0.5rem 0 0.25rem;
    border-top: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  }

  .header .navbar-nav {
    width: 100%;
    margin-right: 0 !important;
    gap: 0.25rem;
  }

  .header .navbar-nav > .nav-item,
  .header .navbar-nav > .nav-item > .nav-link {
    width: 100%;
  }

  .header .navbar-nav > .nav-item > .nav-link {
    display: flex;
    align-items: center;
    min-height: 2.5rem;
    padding: 0.5rem 0.75rem;
    border-radius: var(--ui-radius-md, 12px);
    gap: 0.5rem;
  }

  .header .navbar-nav > .nav-item > .nav-link:hover,
  .header .navbar-nav > .nav-item > .nav-link.active,
  .header .navbar-nav > .nav-item > .nav-link:focus-visible {
    background: var(--ui-accent-soft, rgba(74, 158, 255, 0.12));
  }

  .header .navbar-utilities {
    align-items: center;
    flex-direction: row;
    gap: 0.5rem;
    width: 100%;
    margin-left: 0 !important;
    padding-top: 0.5rem;
    border-top: 1px solid var(--ui-border, rgba(74, 158, 255, 0.22));
  }

  .header .navbar-utilities .bd-mode-toggle {
    flex: 1 1 auto;
  }

  .header .navbar-utilities .bd-mode-toggle .nav-link {
    display: flex;
    align-items: center;
    width: 100%;
    min-height: 2.5rem;
    gap: 0.35rem;
  }

  .header .navbar-utilities .account-menu {
    flex: 0 0 auto;
    margin-left: auto;
  }

  .header .navbar-utilities .account-menu .nav-utility-button {
    width: auto;
    min-width: 2.5rem;
    min-height: 2.5rem;
  }

  .header .navbar-utilities .dropdown-menu {
    width: max-content;
    max-width: calc(100vw - 1.5rem);
  }

  .header .navbar-toggler {
    border-radius: var(--ui-radius-md, 12px);
  }
}
</style>
