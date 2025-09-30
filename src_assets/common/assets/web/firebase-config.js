// Firebase配置和初始化
import { initializeApp } from 'firebase/app';
import { getAnalytics, logEvent } from 'firebase/analytics';

const firebaseConfig = {
  apiKey: "AIzaSyD7VDKZyA1PG6mCO2QdPAUXIziVfTahV0g",
  authDomain: "sunshine-foundation-3c551.firebaseapp.com",
  projectId: "sunshine-foundation-3c551",
  storageBucket: "sunshine-foundation-3c551.firebasestorage.app",
  messagingSenderId: "72108120145",
  appId: "1:72108120145:web:d8f4933fd63766290a4070",
  measurementId: "G-F20721ZDL1"
};

// 初始化Firebase
let app = null;
let analytics = null;

export function initFirebase() {
  try {
    app = initializeApp(firebaseConfig);
    analytics = getAnalytics(app);
    console.log('Firebase Analytics 初始化成功');
    return { app, analytics };
  } catch (error) {
    console.error('Firebase 初始化失败:', error);
    return null;
  }
}

// 记录自定义事件
export function trackEvent(eventName, parameters = {}) {
  if (analytics) {
    try {
      logEvent(analytics, eventName, parameters);
      console.log(`事件已记录: ${eventName}`, parameters);
    } catch (error) {
      console.error('记录事件失败:', error);
    }
  }
}

// 预定义的事件跟踪函数
export const trackEvents = {
  // 页面访问
  pageView: (pageName) => trackEvent('page_view', { page_name: pageName }),
  
  // 用户操作
  userAction: (action, details = {}) => trackEvent('user_action', { action, ...details }),
  
  // 应用管理
  appAdded: (appName) => trackEvent('app_added', { app_name: appName }),
  appDeleted: (appName) => trackEvent('app_deleted', { app_name: appName }),
  appLaunched: (appName) => trackEvent('app_launched', { app_name: appName }),
  
  // 配置更改
  configChanged: (configSection, setting) => trackEvent('config_changed', { 
    section: configSection, 
    setting 
  }),
  
  // 错误事件
  errorOccurred: (errorType, errorMessage) => trackEvent('error_occurred', { 
    error_type: errorType, 
    error_message: errorMessage 
  }),
  
  // 版本更新
  versionChecked: (currentVersion, latestVersion) => trackEvent('version_checked', { 
    current_version: currentVersion, 
    latest_version: latestVersion 
  }),
  
  // 配对操作
  devicePaired: (deviceType) => trackEvent('device_paired', { device_type: deviceType }),
  deviceUnpaired: (deviceType) => trackEvent('device_unpaired', { device_type: deviceType }),
  
  // 流媒体相关
  streamStarted: (quality, encoder) => trackEvent('stream_started', { 
    quality, 
    encoder 
  }),
  streamEnded: (duration, reason) => trackEvent('stream_ended', { 
    duration, 
    reason 
  })
};

export { analytics };

