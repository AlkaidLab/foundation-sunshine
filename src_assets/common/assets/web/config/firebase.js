const firebaseConfig = {
  apiKey: 'AIzaSyD7VDKZyA1PG6mCO2QdPAUXIziVfTahV0g',
  authDomain: 'sunshine-foundation-3c551.firebaseapp.com',
  projectId: 'sunshine-foundation-3c551',
  storageBucket: 'sunshine-foundation-3c551.firebasestorage.app',
  messagingSenderId: '72108120145',
  appId: '1:72108120145:web:d8f4933fd63766290a4070',
  measurementId: 'G-F20721ZDL1',
}

const MAX_PENDING_EVENTS = 50
const ANALYTICS_IDLE_TIMEOUT = 2000

let analytics = null
let analyticsModule = null
let initialization = null
let initializationScheduled = false
let analyticsDisabled = false
const pendingEvents = []

const normalizeParamValue = (value) => {
  let normalizedValue = value

  if (Array.isArray(value)) {
    normalizedValue = value.join(', ')
  } else if (typeof value === 'object' && value) {
    try {
      normalizedValue = JSON.stringify(value)
    } catch {
      normalizedValue = '[unserializable]'
    }
  }

  if (typeof normalizedValue === 'string' && normalizedValue.length > 100) {
    return `${normalizedValue.substring(0, 97)}...`
  }
  return normalizedValue
}

const sanitizeParams = (params = {}) => Object.fromEntries(
  Object.entries(params ?? {}).map(([key, value]) => [key, normalizeParamValue(value)])
)

const sendEvent = ({ eventName, params }) => {
  if (!analytics || !analyticsModule) return
  try {
    analyticsModule.logEvent(analytics, eventName, params)
  } catch (error) {
    console.warn('记录 Firebase 事件失败:', error)
  }
}

const flushPendingEvents = () => {
  pendingEvents.splice(0).forEach(sendEvent)
}

export function initFirebase() {
  initializationScheduled = false
  if (initialization) return initialization

  initialization = (async () => {
    try {
      const [appModule, nextAnalyticsModule] = await Promise.all([
        import('firebase/app'),
        import('firebase/analytics'),
      ])

      if (nextAnalyticsModule.isSupported && !(await nextAnalyticsModule.isSupported())) {
        analyticsDisabled = true
        pendingEvents.length = 0
        return null
      }

      const app = appModule.getApps().length ? appModule.getApp() : appModule.initializeApp(firebaseConfig)
      analyticsModule = nextAnalyticsModule
      analytics = nextAnalyticsModule.getAnalytics(app)
      flushPendingEvents()
      return { app, analytics }
    } catch (error) {
      analyticsDisabled = true
      pendingEvents.length = 0
      console.warn('Firebase 初始化失败:', error)
      return null
    }
  })()

  return initialization
}

const scheduleInitialization = () => {
  if (analyticsDisabled || initialization || initializationScheduled) return
  initializationScheduled = true

  const initialize = () => { void initFirebase() }
  if (typeof globalThis.requestIdleCallback === 'function') {
    globalThis.requestIdleCallback(initialize, { timeout: ANALYTICS_IDLE_TIMEOUT })
  } else {
    globalThis.setTimeout(initialize, 0)
  }
}

export function trackEvent(eventName, params = {}) {
  if (analyticsDisabled) return

  const event = { eventName, params: sanitizeParams(params) }
  if (analytics && analyticsModule) {
    sendEvent(event)
    return
  }

  if (pendingEvents.length >= MAX_PENDING_EVENTS) pendingEvents.shift()
  pendingEvents.push(event)
  scheduleInitialization()
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
