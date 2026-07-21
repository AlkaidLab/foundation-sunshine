import i18n from './config/i18n.js'

// must import even if not implicitly using here
// https://github.com/aurelia/skeleton-navigation/issues/894
// https://discourse.aurelia.io/t/bootstrap-import-bootstrap-breaks-dropdown-menu-in-navbar/641/9
// 导入 Bootstrap 并手动设置到全局对象（ES 模块不会自动注册到 window）
import * as bootstrap from 'bootstrap'

// 将 Bootstrap 设置到全局对象，以便在组件中使用
if (typeof window !== 'undefined') {
  window.bootstrap = bootstrap
}

const enableTopLevelViewTransitions = () => {
    let embedded = false
    try {
        embedded = window.self !== window.top
    } catch {
        embedded = true
    }

    if (embedded || document.querySelector('style[data-sunshine-view-transition]')) return

    const style = document.createElement('style')
    style.dataset.sunshineViewTransition = 'true'
    style.textContent = '@view-transition { navigation: auto; }'
    document.head.appendChild(style)
}

export function initApp(app, config) {
    enableTopLevelViewTransitions()
    //Wait for locale initialization, then render
    i18n().then(i18n => {
        app.use(i18n);
        app.provide('i18n', i18n.global)
        app.mount('#app');
        if (config) {
            config(app)
        }
    });
}
