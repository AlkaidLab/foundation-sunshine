<template>
  <nav class="navbar navbar-light navbar-expand-lg navbar-background header">
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
        <ul class="navbar-nav me-auto mb-2 mb-lg-0">
          <li v-for="item in navItems" :key="item.path" class="nav-item">
            <a class="nav-link" :class="{ active: isActive(item.path) }" :href="item.path">
              <i :class="['fas', 'fa-fw', item.icon]"></i> {{ $t(item.label) }}
            </a>
          </li>
          <li class="nav-item">
            <ThemeToggle />
          </li>
        </ul>
      </div>
    </div>
  </nav>
</template>

<script setup>
import { onMounted, onUnmounted, ref } from 'vue'
import ThemeToggle from '../common/ThemeToggle.vue'
import { useBackground } from '../../composables/useBackground.js'

// 导航项配置
const navItems = Object.freeze([
  { path: '/', icon: 'fa-home', label: 'navbar.home' },
  { path: '/pin', icon: 'fa-unlock', label: 'navbar.pin' },
  { path: '/apps', icon: 'fa-stream', label: 'navbar.applications' },
  { path: '/config', icon: 'fa-cog', label: 'navbar.configuration' },
  { path: '/password', icon: 'fa-user-shield', label: 'navbar.password' },
  { path: '/troubleshooting', icon: 'fa-info', label: 'navbar.troubleshoot' },
])

// 使用背景管理 composable
const { loadBackground, addDragListeners } = useBackground()

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
  background-color: #f9d86bee;
  box-shadow: 0 4px 15px rgba(0, 0, 0, 0.2), 0 2px 8px rgba(0, 0, 0, 0.15);
}

.brand-enhanced {
  transition: transform 0.3s ease;
}

.brand-enhanced:hover {
  transform: scale(1.05) rotate(-5deg);
}
</style>

<style>
.header .nav-link {
  color: rgba(0, 0, 0, 0.65) !important;
  transition: color 0.2s ease, font-weight 0.2s ease;
}

.header .nav-link:hover,
.header .nav-link.active {
  color: rgb(0, 0, 0) !important;
}

.header .navbar-toggler {
  color: rgba(var(--bs-dark-rgb), 0.65) !important;
  border: var(--bs-border-width) solid rgba(var(--bs-dark-rgb), 0.15) !important;
}

.header .navbar-toggler-icon {
  --bs-navbar-toggler-icon-bg: url("data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 30 30'%3e%3cpath stroke='rgba%2833, 37, 41, 0.75%29' stroke-linecap='round' stroke-miterlimit='10' stroke-width='2' d='M4 7h22M4 15h22M4 23h22'/%3e%3c/svg%3e") !important;
}

.form-control::placeholder {
  opacity: 0.5;
}
</style>
