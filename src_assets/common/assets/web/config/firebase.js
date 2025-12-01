// Firebase配置和初始化
import { initializeApp } from 'firebase/app'
import { getAnalytics, logEvent } from 'firebase/analytics'

const firebaseConfig = {
  apiKey: 'AIzaSyD7VDKZyA1PG6mCO2QdPAUXIziVfTahV0g',
  authDomain: 'sunshine-foundation-3c551.firebaseapp.com',
  projectId: 'sunshine-foundation-3c551',
  storageBucket: 'sunshine-foundation-3c551.firebasestorage.app',
  messagingSenderId: '72108120145',
  appId: '1:72108120145:web:d8f4933fd63766290a4070',
  measurementId: 'G-F20721ZDL1',
}

let analytics = null

export function initFirebase() {
  try {
    const app = initializeApp(firebaseConfig)
    analytics = getAnalytics(app)
    return { app, analytics }
  } catch (error) {
    console.error('Firebase 初始化失败:', error)
    return null
  }
}

export function trackEvent(eventName, params = {}) {
  if (!analytics) return

  // 处理参数：数组转字符串，截断过长值
  const sanitized = Object.fromEntries(
    Object.entries(params).map(([k, v]) => {
      let val = Array.isArray(v) ? v.join(', ') : typeof v === 'object' && v ? JSON.stringify(v) : v
      if (typeof val === 'string' && val.length > 100) val = val.substring(0, 97) + '...'
      return [k, val]
    })
  )

  try {
    logEvent(analytics, eventName, sanitized)
  } catch (error) {
    console.error('记录事件失败:', error)
  }
}

export const trackEvents = {
  pageView: (pageName) => trackEvent('page_view', { page_name: pageName }),
  userAction: (action, details = {}) => trackEvent('user_action', { action, ...details }),
  appAdded: (appName) => trackEvent('app_added', { app_name: appName }),
  appDeleted: (appName) => trackEvent('app_deleted', { app_name: appName }),
  configChanged: (section, setting) => trackEvent('config_changed', { section, setting }),
  errorOccurred: (type, message) => trackEvent('error_occurred', { error_type: type, error_message: message }),
  versionChecked: (current, latest) => trackEvent('version_checked', { current_version: current, latest_version: latest }),
  devicePaired: (type) => trackEvent('device_paired', { device_type: type }),
  gpuReported: (info) => trackEvent('gpu_reported', info),
}

export { analytics }
