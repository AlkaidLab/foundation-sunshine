#!/usr/bin/env node
// Builds the per-setting search index of the configuration pages (config/searchIndex.js) from the tab templates.
//
// A setting is a row of a tab that carries its configuration key: a Cfg* row (`k="..."`), or any element with
// `data-cfg="..."` and a `:title`/`:label` made of one locale key. Its place is the tab (file) and the section
// (`<CfgSub id>` page, or the tab page itself). Components of configs/tabs that a template uses are read in place.
//
//   node scripts/gen-config-index.mjs            writes src/settings/searchIndex.js and labels.js of the foundation-ui package
//   node scripts/gen-config-index.mjs --check    exits 1 when the file is not up to date
import { readdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const WEB = resolve(dirname(fileURLToPath(import.meta.url)), '../src_assets/common/assets/web')
const TABS_DIR = join(WEB, 'configs/tabs')
// The shared settings model lives in the foundation-ui package (the sibling checkout `../ds5-ui-m3e`, or FOUNDATION_UI_DIR / FOUNDATION_UI)
const PACKAGE = process.env.FOUNDATION_UI_DIR || process.env.FOUNDATION_UI || resolve(dirname(fileURLToPath(import.meta.url)), '../../ds5-ui-m3e')
const OUTPUT = join(PACKAGE, 'src/settings/searchIndex.js')
const LABELS_OUTPUT = join(PACKAGE, 'src/settings/labels.js')
const LOCALE_DIR = join(WEB, 'public/assets/locale')
// locale key families of the settings area that the list pane, the search and the recent list show
const LABEL_PREFIXES = ['tabs.', 'navbar.', 'shell.', 'ds5.cards.', 'ds5.groups.', 'config.gamepad_']

// file (relative to configs/tabs) -> tab id; the encoder files are pages of the encoders tab, named by their encoder id
const TAB_FILES = { 'General.vue': 'general', 'Inputs.vue': 'input', 'Audio.vue': 'audio', 'Network.vue': 'network', 'Files.vue': 'files', 'Display.vue': 'display' }
const ENCODER_FILES = { 'NvidiaNvencEncoder.vue': 'nv', 'IntelQuickSyncEncoder.vue': 'qsv', 'AmdAmfEncoder.vue': 'amd', 'VideotoolboxEncoder.vue': 'vt', 'SoftwareEncoder.vue': 'sw' }
// the first place of the path of a setting where the rail is not named like the tab (输入 is the rail 输入与外设)
const PATH_ROOT = { input: 'shell.input_peripherals' }
const rootOf = (tab) => PATH_ROOT[tab] || `tabs.${tab}`
const pathOf = (ctx) => [rootOf(ctx.tab), ...(ctx.pathKeys || []), ...(ctx.subKey ? [ctx.subKey] : [])]
const PLACE_ICONS = { 'input:gamepad': 'sports_esports', 'input:keyboard': 'keyboard', 'input:mouse': 'mouse', 'input:usb': 'usb' }
const TAB_ICONS = { general: 'settings', input: 'mouse', av: 'tv', network: 'lan', files: 'folder', advanced: 'tune', encoders: 'memory', display: 'desktop_windows', audio: 'volume_up' }
const ROW_TAGS = new Set(['CfgSwitch', 'CfgChoice', 'CfgField', 'CfgExpert', 'CfgSlider', 'CfgSteps', 'CfgCards', 'CfgPath', 'CfgDevice'])
// components that are one setting each and carry no row attributes
// [key, title locale key, tab, section, section locale key]
const EXTRA_SETTINGS = [
  ['global_prep_cmd', 'config.global_prep_cmd', 'general', 'prep', 'config.global_prep_cmd'],
  ['resolutions', 'config.resolutions', 'display', 'modes', 'shell.display_modes'],
  // MWebAccess draws both: who may open the Web UI, and the bind address in its advanced row
  ['origin_web_ui_allowed', 'config.origin_web_ui_allowed', 'network', '', ''],
  ['bind_address', 'config.bind_address', 'network', '', 'tabs.advanced'],
]
// components of the Web UI that draw several settings at once (the shared virtual display control): [[key, title locale key]]
const { VDD_SEARCH_KEYS, VDD_PARTS } = await import(pathToFileURL(join(PACKAGE, 'src/settings/virtualDisplay.js')).href)
// the control draws the settings of the part a page names (`parts="output"`); it is not read in place, a page that draws a part has those settings
const groupKeys = (tag) => (tag.name === 'VirtualDisplayPanel' ? VDD_SEARCH_KEYS.filter(([id]) => (VDD_PARTS[tag.attrs.parts] || []).includes(id)) : [])
const NOT_READ_IN_PLACE = new Set(['VirtualDisplayPanel'])
const SETTING_COMPONENTS = { DisplayPreparationPicker: ['display_device_prep', 'config.display_device_prep'], VulkanHdrBridgeCard: ['vdd_vulkan_hdr_bridge', 'config.vdd_vulkan_hdr_bridge'] }

/** Tags of a template: [{ name, attrs, closing, selfClosing }] (quotes respected, so `>` inside an attribute is fine). */
export function tokenize(template) {
  const tags = []
  let i = 0
  while (i < template.length) {
    const open = template.indexOf('<', i)
    if (open < 0) break
    if (template.startsWith('<!--', open)) { i = template.indexOf('-->', open) + 3 || template.length; continue }
    const first = template[open + 1]
    if (!/[A-Za-z/]/.test(first || '')) { i = open + 1; continue }
    let j = open + 1
    let quote = ''
    while (j < template.length) {
      const c = template[j]
      if (quote) { if (c === quote) quote = '' } else if (c === '"' || c === "'") quote = c
      else if (c === '>') break
      j++
    }
    const raw = template.slice(open + 1, j)
    const closing = raw.startsWith('/')
    const body = closing ? raw.slice(1) : raw
    const name = /^[A-Za-z][\w:.-]*/.exec(body)?.[0] || ''
    tags.push({ name, attrs: parseAttrs(body.slice(name.length)), closing, selfClosing: raw.endsWith('/') })
    i = j + 1
  }
  return tags
}

function parseAttrs(text) {
  const attrs = {}
  const re = /([:@#\w.-]+)(?:\s*=\s*("([^"]*)"|'([^']*)'))?/g
  let m
  while ((m = re.exec(text))) attrs[m[1]] = m[3] ?? m[4] ?? ''
  return attrs
}

/** The one locale key of an expression like `$t('config.x')` or `$tp('config.x', ...)`, else ''. */
export function localeKeyOf(expression = '') {
  return /^\s*\$tp?\(\s*['"]([\w.]+)['"]\s*(?:,[^)]*)?\)\s*$/.exec(expression)?.[1] || ''
}

const templateOf = (source) => {
  const start = source.indexOf('\n<template>\n')
  if (start < 0) return ''
  const end = source.lastIndexOf('\n</template>')
  return source.slice(start + 1, end < 0 ? source.length : end + 12)
}
const importsOf = (source) => {
  const map = {}
  for (const m of source.matchAll(/import\s+(\w+)\s+from\s+['"]([^'"]+\.vue)['"]/g)) map[m[1]] = m[2]
  return map
}

export function buildConfigIndex(tabsDir = TABS_DIR) {
  const entries = []
  const seen = new Set()
  const icons = {}

  const scan = (file, ctx, stack = []) => {
    const source = readFileSync(file, 'utf8')
    const imports = importsOf(source)
    const tags = tokenize(templateOf(source))
    const local = []
    for (const tag of tags) {
      if (tag.closing) {
        if (tag.name === 'CfgSub' || tag.name === 'CfgTop') ctx = local.pop() || ctx
        continue
      }
      if (tag.name === 'CfgRow' && tag.attrs.id) icons[`${ctx.tab}:${tag.attrs.id}`] = tag.attrs.icon
      if (tag.name === 'CfgSub') {
        local.push(ctx)
        // an inline section is part of the tab page: its settings are found on the page (no sub), the path still names the section
        ctx = { ...ctx, sub: 'inline' in tag.attrs ? '' : tag.attrs.id, subKey: localeKeyOf(tag.attrs[':title']) }
        if (tag.selfClosing) ctx = local.pop()
        continue
      }
      if (tag.name === 'CfgTop') { local.push(ctx); ctx = { ...ctx, sub: '', subKey: '' }; continue }
      let id = ''
      let title = ''
      if (ROW_TAGS.has(tag.name) && tag.attrs.k) { id = tag.attrs.k; title = tag.attrs.label || `config.${id}` }
      else if (tag.name === 'AutomaticNumberSetting' && tag.attrs.id) { id = tag.attrs.id; title = tag.attrs['label-key'] }
      else if (SETTING_COMPONENTS[tag.name]) [id, title] = SETTING_COMPONENTS[tag.name]
      else if (tag.attrs['data-cfg']) { id = tag.attrs['data-cfg']; title = localeKeyOf(tag.attrs[':title'] || tag.attrs[':label']) }
      for (const [groupId, groupTitle] of groupKeys(tag)) {
        if (!seen.has(groupId)) { seen.add(groupId); entries.push({ id: groupId, title: groupTitle, path: pathOf(ctx), tab: ctx.tab, sub: ctx.sub }) }
      }
      if (id && title && !seen.has(id)) {
        seen.add(id)
        entries.push({ id, title, path: pathOf(ctx), tab: ctx.tab, sub: ctx.sub })
      }
      const target = NOT_READ_IN_PLACE.has(tag.name) ? '' : imports[tag.name]
      if (target && /^\.\.?\//.test(target) && !stack.includes(target)) {
        const path = resolve(dirname(file), target)
        if (path.startsWith(tabsDir)) scan(path, ctx, [...stack, target])
      }
    }
  }

  for (const [name, tab] of Object.entries(TAB_FILES)) scan(join(tabsDir, name), { tab, sub: '', subKey: '' })
  for (const [name, sub] of Object.entries(ENCODER_FILES)) scan(join(tabsDir, 'encoders', name), { tab: 'display', sub: 'encoders', subKey: `tabs.${sub}`, pathKeys: ['shell.display_encoders'] })

  // settings that are edited by a component of their own (a table, a list) and so carry no row attribute
  for (const [id, title, tab, sub, subKey] of EXTRA_SETTINGS) {
    if (!seen.has(id)) entries.push({ id, title, path: [rootOf(tab), ...(subKey ? [subKey] : [])], tab, sub })
  }

  return entries.map(({ id, title, path, tab, sub }) => ({
    id,
    title,
    path,
    icon: PLACE_ICONS[`${tab}:${sub}`] || icons[`${tab}:${sub}`] || TAB_ICONS[tab],
    keywords: [id === id.replace(/_/g, ' ') ? id : `${id} ${id.replace(/_/g, ' ')}`, SEARCH_WORDS[id]].filter(Boolean).join(' '),
    route: 'config',
    params: { tab, ...(sub ? { sub } : {}), focus: id },
  }))
}

// words a person types for a setting that its label does not contain (who may open the Web UI from a phone: 局域网, 公网, 手机, 绑定, IP, 访问)
const SEARCH_WORDS = {
  origin_web_ui_allowed: '访问范围 局域网 公网 外网 本机 手机 平板 访问 远程 权限 lan wan remote phone mobile access scope web ui',
  bind_address: '绑定地址 绑定 监听 局域网 手机 访问 IP 地址 网卡 bind listen lan remote phone ip address',
}

export function renderIndexFile(index) {
  const lines = index.map((entry) => `  ${JSON.stringify(entry)},`)
  return `// Generated by scripts/gen-config-index.mjs from the tab templates; do not edit. One entry per setting of the
// configuration: { id (the configuration key), title (locale key), path (locale keys of the places above it), icon,
// keywords, route, params: { tab, sub?, focus } }. The search bar of the list pane opens the tab and the section.
export const SEARCH_INDEX = [
${lines.join('\n')}
]
`
}

const lookupKey = (locale, key) => key.split('.').reduce((node, part) => (node && typeof node === 'object' ? node[part] : undefined), locale)
const flatten = (node, prefix = '', out = {}) => {
  for (const [k, v] of Object.entries(node || {})) {
    if (v && typeof v === 'object') flatten(v, `${prefix}${k}.`, out)
    else if (typeof v === 'string') out[`${prefix}${k}`] = v
  }
  return out
}

/** { locale: { key: text } } of the keys the settings area needs, for every locale file (English fills a gap). */
export function buildLabels(index, extraKeys = []) {
  const wanted = new Set(extraKeys)
  for (const e of index) { wanted.add(e.title); for (const p of e.path) wanted.add(p) }
  const read = (name) => JSON.parse(readFileSync(join(LOCALE_DIR, name), 'utf8'))
  const en = read('en.json')
  const enFlat = flatten(en)
  for (const key of Object.keys(enFlat)) if (LABEL_PREFIXES.some((p) => key.startsWith(p))) wanted.add(key)
  const keys = [...wanted].sort()
  const out = {}
  for (const file of readdirSync(LOCALE_DIR).filter((f) => f.endsWith('.json')).sort()) {
    const name = file.replace(/\.json$/, '')
    const locale = name === 'en' ? en : read(file)
    const table = {}
    for (const key of keys) {
      // a setting that has only platform variants is read by its Windows text
      const text = [key, `${key}_windows`, `${key}_unix`].map((k) => lookupKey(locale, k)).find((v) => typeof v === 'string')
      if (typeof text === 'string' && (name === 'en' || text !== lookupKey(en, key))) table[key] = text
      else if (name === 'en') { const fallback = [`${key}_windows`, `${key}_unix`].map((k) => lookupKey(en, k)).find((v) => typeof v === 'string'); if (fallback) table[key] = fallback }
    }
    out[name] = table
  }
  return out
}

export function renderLabelsFile(labels) {
  const lines = Object.entries(labels).map(([name, table]) => `  ${JSON.stringify(name)}: ${JSON.stringify(table)},`)
  return `// Generated by scripts/gen-config-index.mjs from the Web UI locale files; do not edit. The texts of the settings area
// (the search index, the categories, the list pane) for every interface language. A key a language does not translate
// is absent there: labelOf falls back to English.
export const LABELS = {
${lines.join('\n')}
}
const fallbackOf = (locale) => (locale && LABELS[locale]) || LABELS[String(locale || '').replace(/-/g, '_')] || LABELS[String(locale || '').split(/[_-]/)[0]] || LABELS.en
/** The text table of an interface language. */
export const labelsFor = (locale) => fallbackOf(locale)
/** The text of a locale key in a language, English when the language lacks it, the key itself when nobody has it. */
export const labelOf = (locale, key) => fallbackOf(locale)[key] ?? LABELS.en[key] ?? key
`
}

if (import.meta.url === pathToFileURL(process.argv[1] || '').href) {
  const index = buildConfigIndex()
  const { MODEL_LABEL_KEYS } = await import(pathToFileURL(join(PACKAGE, 'src/settings/model.js')).href)
  const files = [[OUTPUT, renderIndexFile(index)], [LABELS_OUTPUT, renderLabelsFile(buildLabels(index, MODEL_LABEL_KEYS))]]
  if (process.argv.includes('--check')) {
    for (const [path, text] of files) {
      let current = ''
      try { current = readFileSync(path, 'utf8') } catch { /* missing */ }
      if (current !== text) { console.error(`${path} is out of date: run node scripts/gen-config-index.mjs`); process.exit(1) }
    }
  } else {
    for (const [path, text] of files) writeFileSync(path, text)
    console.log(`settings index: ${index.length} settings`)
  }
}
