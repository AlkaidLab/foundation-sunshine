/**
 * Logout composable
 */
export function useLogout() {
  /**
   * @param { { onLocalhost?: () => void } } [opts]
   */
  const logout = (opts = {}) => {
    const xhr = new XMLHttpRequest()
    xhr.open('GET', '/api/logout?t=' + Date.now(), true)
    xhr.setRequestHeader('Authorization', 'Basic ' + btoa('logout:logout'))

    // 既然浏览器可能会卡在 401 重试里不回调 JS，
    // 那我们就不等了。设定 200ms 后，无论如何必须跳回首页。
    // 这能强制打断浏览器的 XHR 死循环。
    const watchdog = setTimeout(() => {
      // 如果到了这里，说明 200 没触发，或者浏览器卡在 401 了
      // 强行中止请求，跳转首页
      try { xhr.abort() } catch(e) {}
      window.location.href = '/'
    }, 200)

    xhr.onreadystatechange = () => {
      // 只有一种情况我们取消跳转：后端明确返回了 200 (Localhost)
      if (xhr.readyState === 4 && xhr.status === 200) {
        clearTimeout(watchdog)
        opts.onLocalhost?.()   // 执行 Localhost 逻辑
      }
    }
    xhr.send()
  }
  return { logout }
}