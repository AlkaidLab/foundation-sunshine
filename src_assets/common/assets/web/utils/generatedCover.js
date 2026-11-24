/**
 * 生成封面：六种风格、宿主的预览地址、应用当前封面的来源
 *
 * 契约见 docs/generated-covers-api.md：GET /api/apps 给每个应用加 cover-source / cover-effective-style /
 * cover-portrait-url / cover-landscape-url；GET /api/apps/cover-preview 渲染指定风格的一张图。
 * cover-*-url 指向 /appasset（串流端口，客户端证书），Web UI 在配置端口，所以这里的缩略图一律走 cover-preview。
 */

export const COVER_STYLES = Object.freeze(['tonal', 'monogram', 'icon', 'editorial', 'glass', 'geometric'])
export const DEFAULT_COVER_STYLE = 'tonal'
export const COVER_STYLE_AUTO = 'auto'

// 概念稿里的字母，界面上只作辅助标记
export const COVER_STYLE_LETTER = Object.freeze({ tonal: 'A', monogram: 'B', icon: 'C', editorial: 'D', glass: 'E', geometric: 'F' })

export const isCoverStyle = (value) => COVER_STYLES.includes(value)

/** the style name of a value of the configuration or of an application: anything unknown is the default (the host does the same) */
export const normalizeCoverStyle = (value) => (isCoverStyle(value) ? value : DEFAULT_COVER_STYLE)

/** an application's own choice: 'auto' (follow the global style) or one of the six */
export const appCoverChoice = (app) => {
  const value = app?.['cover-style']
  return isCoverStyle(value) ? value : COVER_STYLE_AUTO
}

/**
 * the host's own placeholder picture as the default Xbox entry and older versions wrote it (box.png, ./assets/box.png): not a picture of the person's.
 * An absolute path to that file is told by the host (cover-source), which knows where its assets are.
 */
export const isPlaceholderImagePath = (path) => {
  let value = String(path ?? '').trim().replace(/^"+|"+$/g, '').replace(/\\/g, '/').toLowerCase()
  while (value.startsWith('./')) value = value.slice(2)
  if (value.startsWith('assets/')) value = value.slice(7)
  return value === '' || value === 'box.png'
}

/** the host's id of an application, read from its cover URL (the list has no id field of its own) */
export const coverAppId = (app) => {
  const url = app?.['cover-portrait-url'] || app?.['cover-landscape-url'] || app?.cover?.portrait || ''
  if (!url) return ''
  try {
    return new URL(url, 'http://host.invalid').searchParams.get('appid') || ''
  } catch {
    return ''
  }
}

/**
 * Where a generated cover is shown by the Web UI. `name` is part of the address on purpose: the host renders from the saved name and the
 * browser may keep a preview for a minute, so a rename must not show the old text.
 */
export const coverPreviewUrl = (appId, style, orientation = 'portrait', name = '') => {
  if (!appId || !isCoverStyle(style)) return ''
  const query = new URLSearchParams({ appid: appId, style, orientation })
  if (name) query.set('n', name)
  return `/api/apps/cover-preview?${query.toString()}`
}

/** 'generated' | 'poster' | 'xbox' | '' (a host that does not report it, or a new application) */
export const coverSourceOf = (app) => {
  const value = app?.['cover-source'] || app?.cover?.source || ''
  return value === 'generated' || value === 'poster' || value === 'xbox' ? value : ''
}

/** the style an application shows now (its own choice, else the global one), from the list's own report */
export const coverEffectiveStyle = (app) => {
  const value = app?.['cover-effective-style'] || app?.cover?.style
  return isCoverStyle(value) ? value : ''
}

// fields of the list that describe the cover: the host sets them, it takes none of them back (it drops them on save)
export const COVER_REPORT_KEYS = Object.freeze(['cover-source', 'cover-effective-style', 'cover-portrait-url', 'cover-landscape-url', 'cover'])
export const withoutCoverReport = (app) => {
  const copy = { ...app }
  for (const key of COVER_REPORT_KEYS) delete copy[key]
  return copy
}
