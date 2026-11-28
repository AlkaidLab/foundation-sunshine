// 图形 API in the menu and an API argument among the custom launch arguments that asks for the other DirectX: both would reach the game, so a save asks which one goes.
const MENU = Object.freeze({
  genshin: { key: 'dxVersion', versions: { dx11: 11, dx12: 12 } },
  starrail: { key: 'dxVersion', versions: { dx11: 11, dx12: 12 } },
  zzz: { key: 'graphicsApi', versions: { '10.1': 11, '11.0': 11, '11.1': 11, '11.1ns': 11, 12: 12 } },
  hi3: { key: 'graphicsApi', versions: { '10.1': 11, '11.0': 11, '11.1': 11, '11.1ns': 11, 12: 12 } },
})
const ARGUMENTS = Object.freeze({ '-force-d3d11': 11, '-force-d3d12': 12, '-use-d3d12': 12 })
// an argument as the host tells them apart (quote_args): from a quote to the one that closes it, or up to a blank or a quote
const WORD = /"[^"]*"|[^\s"]+/g
const bare = (word) => word.replace(/^"(.*)"$/, '$1').toLowerCase()

/** null, or { key: the menu's item, value: what it holds, args: the arguments that ask for the other DirectX, as typed without quotes } */
export function graphicsConflict(game, settings) {
  const menu = MENU[game]
  if (!menu) return null
  const value = settings?.[menu.key]
  const version = menu.versions[value]
  if (!version) return null
  const args = (String(settings.customArgs ?? '').match(WORD) || []).filter((w) => ARGUMENTS[bare(w)] && ARGUMENTS[bare(w)] !== version)
  return args.length ? { key: menu.key, value, args: [...new Set(args.map((w) => w.replace(/"/g, '')))] } : null
}

const withoutArguments = (text, args) => String(text).split(/\r\n|\r|\n/).map((line) => {
  let cut = false
  const out = line.replace(WORD, (word) => {
    if (!args.some((a) => a.toLowerCase() === bare(word))) return word
    cut = true
    return ''
  })
  // a line nothing was taken from stays as typed
  return cut ? out.replace(/[ \t]{2,}/g, ' ').trim() : line
}).filter((line) => line.trim()).join('\n')

/** Changes `settings` in place. 'menu': the menu's item is let go (it follows the game again); 'args': the arguments go from the custom ones. */
export function resolveGraphicsConflict(settings, conflict, choice) {
  if (choice === 'menu') {
    delete settings[conflict.key]
    return
  }
  const rest = withoutArguments(settings.customArgs, conflict.args)
  if (rest) settings.customArgs = rest
  else delete settings.customArgs
}

const dialog = async (options) => {
  const { useDialogs } = await import('foundation-ui/dialog')
  return useDialogs().ask(options)
}

/** Asks which one goes: 'menu', 'args', or null when the dialog was closed (Escape, the backdrop, back): nothing is saved then. */
export async function askGraphicsConflict(conflict, api, texts, { ask = dialog } = {}) {
  try {
    await ask({ title: texts.apiConflictTitle, message: texts.apiConflictText(api, conflict.args.join(texts.listSep)), type: 'warning', confirmText: texts.apiConflictMenu, cancelText: texts.apiConflictArgs, distinguish: true })
    return 'menu'
  } catch (action) {
    return action === 'cancel' ? 'args' : null
  }
}
