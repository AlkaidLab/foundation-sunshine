// SPDX-License-Identifier: GPL-3.0-only
// The docs of the unified search as the Web UI shell builds them (views/WebShell.vue), without the shell: the settings of this product's own data with
// their aliases, the categories of the settings list, the pages and rails of the navigation, the actions, and the tiles of the overview.
// The pages and rails mirror WebShell.vue's PAGES and RAIL (a test of searchEval.test.js reads the shell and checks each label key is still there).
import './foundationUi.mjs'
const { SEARCH_INDEX } = await import('../../config/settingsData/index.js')
const provider = await import('foundation-ui/settings/provider.js')
const { buildSettingsEntries } = await import('foundation-ui/settings/search.js')
const { settingsGroups, SETTINGS_CATEGORIES, STANDALONE_CATEGORIES } = await import('foundation-ui/settings/model.js')
const { rowKeywords } = await import('foundation-ui/settings/rowLines.js')
const { buildIndex, search } = await import('foundation-ui/search/index.js')
const { settingDocs, tileDocs } = await import('foundation-ui/search/docs.js')
const { actionDocs, searchTexts, ACTION_DEFS } = await import('foundation-ui/search/texts.js')

export { SEARCH_INDEX, buildIndex, search }

// WebShell.vue PAGES with an icon, and the label key of each
export const NAV_PAGES = Object.freeze({
  pin: ['stream', 'navbar.clients'], apps: ['stream', 'navbar.applications'], downloads: ['stream', 'resource_card.client_downloads'],
  gamepad: ['input', 'shell.gamepad'], keyboard: ['input', 'config.input_keyboard_title'], mouse: ['input', 'config.input_mouse_title'], usb: ['input', 'shell.usb_forwarding'],
  'audio-output': ['audio', 'shell.audio_output'], microphone: ['audio', 'shell.microphone'],
  'display-output': ['display', 'shell.display_output'], 'display-modes': ['display', 'shell.display_modes'], 'display-hdr': ['display', 'shell.display_hdr'], 'display-encoders': ['display', 'shell.display_encoders'], vdd: ['display', 'shell.virtual_display'], 'display-advanced': ['display', 'shell.display_advanced'],
  troubleshooting: ['system', 'shell.logs_diagnostics'],
})
// WebShell.vue RAIL: the navigation bar
export const NAV_RAILS = Object.freeze([['overview', 'home', 'navbar.home'], ['stream', 'cast_connected', 'shell.stream'], ['config', 'tune', 'shell.stream_settings'], ['input', 'sports_esports', 'shell.input_peripherals'], ['audio', 'volume_up', 'shell.audio'], ['display', 'desktop_windows', 'shell.display'], ['system', 'build', 'shell.system'], ['help', 'help', 'shell.help_about']])
const ROW_OF_PAGE = { pin: 'clients', apps: 'apps', downloads: 'downloads', gamepad: 'gamepad', keyboard: 'keyboard', mouse: 'mouse', usb: 'usb', microphone: 'microphone', 'display-output': 'display-output', 'display-modes': 'display-modes', 'display-encoders': 'display-encoders', 'display-hdr': 'display-hdr', 'audio-output': 'audio-output', vdd: 'vdd' }

export function searchDocsOf(locale = 'zh') {
  const other = locale === 'zh' ? 'en' : 'zh'
  const label = (k) => provider.labelOf(locale, k)
  const alt = (k) => provider.labelOf(other, k)
  const categories = [
    ...settingsGroups({ label }).flatMap((g) => g.items).map((item) => ({ id: item.id, title: item.label, path: label('navbar.configuration'), icon: item.icon, route: item.route, params: item.params, keywords: `${alt(SETTINGS_CATEGORIES[item.category].labelKey)} ${rowKeywords(item.id)}` })),
    ...STANDALONE_CATEGORIES.map((id) => ({ id, title: label(SETTINGS_CATEGORIES[id].labelKey), path: '', icon: SETTINGS_CATEGORIES[id].icon, route: id, keywords: alt(SETTINGS_CATEGORIES[id].labelKey) })),
  ]
  const pages = Object.entries(NAV_PAGES).map(([id, [rail, key]]) => ({ id, title: label(key), icon: 'tune', route: id, path: label(NAV_RAILS.find((r) => r[0] === rail)[2]), keywords: `${alt(key)} ${rowKeywords(ROW_OF_PAGE[id])}` }))
  const rails = NAV_RAILS.map(([id, icon, key]) => ({ id: `rail-${id}`, title: label(key), icon, route: id, path: '', keywords: alt(key), railId: id }))
  const entries = buildSettingsEntries({ index: provider.getSearchIndex(), label, altLabel: alt, aliases: provider.getAliases?.() })
  const handlers = Object.fromEntries(ACTION_DEFS.map((a) => [a.id, () => {}]))
  return [
    ...settingDocs([...rails, ...pages, ...categories, ...entries], { prefix: label('navbar.configuration'), locale }),
    ...actionDocs(locale, handlers, searchTexts(locale).types.action),
    ...tileDocs(locale),
  ]
}
