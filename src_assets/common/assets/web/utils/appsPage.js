// Pure helpers of the applications page.
import { PER_APP_GAMEPAD_MODES } from './gamepadModes.js'

const truthy = (value) => !!value && value !== 'false'

/** Marks shown on an application card: [{ key, icon, count?, title }] in a fixed order. Auto-detach is the default and shows no mark. */
export function appTags(app, translate) {
  const tags = []
  if (truthy(app['exclude-global-prep-cmd'])) tags.push({ key: 'exclude-global-prep-cmd', icon: 'rule', title: translate('apps.tag_skip_prep_desc') })
  const menu = Array.isArray(app['menu-cmd']) ? app['menu-cmd'].length : 0
  if (menu > 0) tags.push({ key: 'menu-cmd', icon: 'list', count: menu, title: `${translate('apps.tag_menu')}: ${menu}` })
  if (truthy(app.elevated)) tags.push({ key: 'elevated', icon: 'shield', title: translate('apps.tag_admin') })
  const mode = PER_APP_GAMEPAD_MODES.find((entry) => entry.value && entry.value === app.gamepad)
  if (mode) tags.push({ key: 'gamepad', icon: 'sports_esports', title: `${translate('apps.gamepad_mode')}: ${translate(mode.labelKey)}` })
  return tags
}

/**
 * `entries` with the one whose key is `from` moved to the place of the one whose key is `to`.
 * Returns the same entries in the new order; an unknown key returns the list unchanged.
 */
export function moveApp(entries, keyOf, from, to) {
  const list = [...entries]
  const source = list.findIndex((entry) => keyOf(entry) === from)
  const target = list.findIndex((entry) => keyOf(entry) === to)
  if (source < 0 || target < 0 || source === target) return list
  const [moved] = list.splice(source, 1)
  list.splice(target, 0, moved)
  return list
}

/** The example command of the environment variables sheet for a platform, or null. */
export function envExampleFor(platform, translate) {
  if (platform === 'windows') {
    return { title: translate('apps.env_qres_example'), code: `cmd /C <${translate('apps.env_qres_path')}>\\QRes.exe /X:%SUNSHINE_CLIENT_WIDTH% /Y:%SUNSHINE_CLIENT_HEIGHT% /R:%SUNSHINE_CLIENT_FPS%` }
  }
  if (platform === 'linux') {
    return { title: translate('apps.env_xrandr_example'), code: 'sh -c "xrandr --output HDMI-1 --mode \\"${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT}\\" --rate ${SUNSHINE_CLIENT_FPS}"' }
  }
  if (platform === 'macos') {
    return { title: translate('apps.env_displayplacer_example'), code: 'sh -c "displayplacer "id:<screenId> res:${SUNSHINE_CLIENT_WIDTH}x${SUNSHINE_CLIENT_HEIGHT} hz:${SUNSHINE_CLIENT_FPS} scaling:on origin:(0,0) degree:0""' }
  }
  return null
}

/** The cards in the order the person chose to see them: the saved order (`custom`), or by name, ascending or descending (the saved order is not changed). */
export function sortCards(cards, mode = 'custom') {
  if (mode !== 'asc' && mode !== 'desc') return cards
  const sign = mode === 'asc' ? 1 : -1
  return [...cards].sort((a, b) => sign * String(a.title).localeCompare(String(b.title), undefined, { numeric: true, sensitivity: 'base' }))
}
