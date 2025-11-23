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

export function initApp(app, config) {
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
