// Words of the Sparxie pages (Chinese and English). The product's other languages fall back to English until the translators have them.
export const sparxieLang = (locale) => (String(locale || 'zh').toLowerCase().startsWith('zh') ? 'zh' : 'en')

const ZH = {
  games: { genshin: '原神', starrail: '崩坏：星穹铁道', zzz: '绝区零' },
  regions: { cn: '国服', bili: 'B 服', global: '国际服' },
  sources: { hoyoplay: '米哈游启动器', 'official-launcher': '官方启动器', bili: 'B 站', steam: 'Steam', epic: 'Epic', folder: '本地查找', manual: '手动选择' },
  // add menu, new page, import page
  addGame: '添加游戏', importProfiles: '从 Sparxie 导入',
  newTitle: '添加游戏', importTitle: '从 Sparxie 导入', create: '创建', cancel: '取消', retry: '重新检查',
  secGame: '游戏', secRegion: '区服', secInstall: '安装位置', secMode: '启动方式', secNameCover: '名称与封面',
  detectedTag: '已检测到', inSparxie: '已在 Sparxie 中', sparxieOnly: '在 Sparxie 中',
  installNone: '没有自动找到，请选择主程序或文件夹', barNoGame: '未选游戏', barNoRegion: '未选区服', barNoInstall: '未选安装位置', needGame: '先选择游戏', needRegion: '先选择区服', needInstall: '先选择安装位置', writeFailed: '无法写入 Sparxie 配置', installChoose: '选择一个安装位置', changeInstall: '更换位置', installFrom: '来自 Sparxie', installFromGame: '启动器登记',
  pickExe: '选择游戏主程序', pickExeText: (names) => names, pickFolder: '选择游戏文件夹', pickFolderText: '从所选文件夹的第一层开始逐层查找',
  scanning: '正在查找…', scanNone: '所选文件夹里没有找到这个游戏', scanTimeout: '查找用时过长，请选择更具体的文件夹', scanFound: (n) => `找到 ${n} 个安装位置，选择一个`,
  mismatchRegion: (found, chosen) => `所选位置是${found}，不是${chosen}`,
  wrongGame: (names) => `这个文件不属于所选游戏，可用文件名：${names}`, isSparxie: '这是 Sparxie 的位置，请选择游戏所在的位置',
  noUser: '这台电脑当前没有已登录的用户', pickFailed: '无法打开选择器', identifyFailed: '无法识别所选位置', createFailed: '创建失败', saveFailed: '保存失败', failedWith: (verb, reason) => `${verb}：${reason}`,
  storeEmpty: 'Sparxie 中没有游戏配置', storeUnused: 'Sparxie 尚未创建配置文件', writeBack: '创建应用时写入 Sparxie 配置', writeBackNew: '创建应用时生成 Sparxie 配置文件并写入',
  writeBackNone: '该游戏已在 Sparxie 中，不修改其配置',
  closeSparxie: '关闭 Sparxie 后才能把游戏写入它的配置', unreadable: 'Sparxie 的配置文件无法读取',
  region: '区服', coverAuto: '官方海报，随版本更新', coverChange: '更换封面',
  appName: '应用名称',
  // the browse list of the Web UI (the host lists its own drives and folders; a browser cannot give the host a path)
  browseTitleFile: '选择游戏主程序', browseTitleFolder: '选择游戏文件夹', browseRoot: '这台电脑', browseUp: '上一级', browseHere: '选择此文件夹', browseEmpty: '这里没有可显示的内容', browseMore: (n) => `只显示前 ${n} 项`, browseFailed: '无法读取这个位置', browseClose: '返回',
  coverFollow: '跟随官方海报', coverFollowText: '随版本更新', coverUpdated: (t) => `已更新 ${t}`, coverNone: '尚未取得官方海报', coverCustom: '自定义封面', coverRestore: '恢复官方海报',
  posterBadge: '海报显示 Sparxie 标志', posterBadgeOfficial: '画在海报右上角', posterBadgeCustom: '画在你的封面右上角（本地 PNG / JPEG）',
  importEmpty: 'Sparxie 中没有配置', importTaken: '已有应用', importCount: (n) => `导入 ${n} 项`, importAction: '导入所选', importFoot: '每个配置创建一个应用，保留其配置',
  openComponent: '打开 Sparxie', createInSparxie: '在 Sparxie 中新建配置', refresh: '刷新', openFailed: '无法打开 Sparxie', alreadyOpen: 'Sparxie 已经打开',
  addLocal: '添加本地游戏',
  addApp: '添加应用', searchGames: '搜索游戏', clearSearch: '清除搜索', noTitle: (q) => `没有找到“${q}”`, save: '保存', editTitle: '编辑游戏',
  gameHint: (title, region) => `识别为${title}${region ? ` · ${region}` : ''}，改用游戏方式添加？`, gameHintAction: '改用游戏方式添加',
  scanSource: { steam: 'Steam', epic: 'Epic', gog: 'GOG', xbox: 'Xbox', ea: 'EA', ubisoft: '育碧', battlenet: '战网', riot: 'Riot', hoyoplay: '米哈游启动器', wegame: 'WeGame', sparxie: 'Sparxie', directory: '文件夹', other: '其他' }, scanAdded: '已添加', scanGameApp: '游戏应用', scanFromSparxie: '来自 Sparxie 的配置',
  secGameSettings: '游戏设置', gsFpsHint: (n) => `游戏本身最高 ${n} 帧，更高的帧率在 Sparxie 设置的「目标帧率」中选择`, gsFollow: '跟随游戏', gsChanged: '已更改', gsBack: '恢复跟随游戏', gsAllFollow: '全部跟随游戏', restoreNote: '只恢复本应用启动时写入的项；游戏运行中自己改动过的项保留游戏的值',
  // editor
  mode: '启动方式', inputTouch: '触控', inputKbm: '键鼠与手柄', inputName: '输入方式', secInput: '输入方式', secSparxie: 'Sparxie 设置', pc: '直接启动', touchNeeds: '没有 Sparxie 组件，这个应用直接启动游戏', kbmNeedsTouch: '已安装的 Sparxie 无法在键鼠与手柄下保留 PC 界面，所以这些 Sparxie 选项对键鼠与手柄不生效，游戏会由我们直接启动。更新 Sparxie 后即可生效', resolutionAuto: '跟随串流显示', gsNotManaged: (game) => `暂不支持修改${game}的游戏内设置`, src: { hoyoplay: '米哈游启动器', steam: 'Steam', epic: 'Epic', bili: 'B 服客户端', sparxie: 'Sparxie', manual: '手动选择' }, modeTouchText: '通过 Sparxie 启动，需要管理员权限', pcHint: '直接启动游戏',
  path: '游戏路径', version: '游戏版本', change: '更改',
  secTouch: '触控启动', secProfile: '配置', secFps: '帧率', secUi: '触控界面', secAdv: '进阶',
  secDisplay: '显示', displayFollow: '跟随串流', displayFixed: '固定', displayFollowNow: (w, h) => `本次串流将以 ${w}×${h} 全屏运行`, displayFollowLater: '随串流设备的分辨率全屏', displayFixedLine: (size, window) => (window ? `游戏将以${size ? ` ${size} ` : ''}${window}运行` : `游戏将以 ${size} 运行`), secQuality: '图像', secAudio: '声音', secLanguage: '语言', secLaunch: '启动', gsRunning: '游戏正在运行，修改从下次启动生效', gsRunningSave: '游戏正在运行，关闭游戏后再保存', listSep: '、', gsUnsupported: (names) => `这个版本的游戏不支持修改：${names}`, gsApply: '启动时应用以下设置', gsNow: '当前', on: '开', off: '关', fpsDisplay: '跟随显示器刷新率', fpsBySparxie: '帧率由 Sparxie 接管',
  resolution: '分辨率', displayMode: '显示模式', display: { fullscreen: '全屏', borderless: '无边框窗口', windowed: '窗口' }, frameRate: '游戏内帧率上限', fpsOpt: (n) => (n === 0 ? '不限' : `${n}`),
  vsync: '垂直同步', renderScale: '渲染精度', preset: '画质', presetOpts: { low: '低', medium: '中', high: '高', ultra: '最高' }, hdr: 'HDR',
  restoreAfter: '应用结束后', restoreOpts: { restore: '恢复原设置', keep: '保留' }, resetDefaults: '恢复游戏默认',
  fpsUnlock: '帧率解锁', fpsCap: '目标帧率', bgLimit: '限制后台帧率', bgFps: '后台帧率',
  scaleOverride: '覆盖界面缩放', scale: '界面缩放', priority: '进程优先级', priorityOpts: ['低于正常', '正常', '高于正常', '高'], followPreset: '帧率档位映射', p30: '30 帧档位', p60: '60 帧档位',
  profileName: '配置名称', profileId: '配置 ID', makeDefault: '设为默认', deleteProfile: '删除配置', openSparxie: '在 Sparxie 中打开', syncNote: '设置与 Sparxie 同步',
  // the statement about what Sparxie does (shown once before the first application, and as the footer of the 触控启动 group)
  noticeTitle: '经 Sparxie 启动的说明', notice: 'Sparxie 会在游戏运行时向游戏进程注入代码并修改其内存，这可能违反游戏的用户协议，由此带来的账号风险由你自行承担。',
  noticeFoot: 'Sparxie 会在游戏运行时向游戏进程注入代码并修改其内存，这可能违反游戏的用户协议，由此带来的账号风险由你自行承担。',
  // game settings editor
  native: '原生', gameResolution: '游戏内分辨率', search: '搜索', searchNone: '没有匹配项', closeSheet: '关闭', outOfRange: (min, max) => `范围 ${min}–${max}`,
  argsHint: '每行可写多个参数。只允许字母、数字和 _ . / : = + -，参数可用英文双引号括起。', argsPlace: '-screen-quality 2',
  argsChar: (line, ch) => `第 ${line} 行的 ${ch === ' ' ? '空格（引号内）' : `“${ch}”`} 不能使用：它在命令行里有特殊含义，可能执行其他命令`, argsQuote: (line) => `第 ${line} 行的引号没有闭合`,
  hints: { gameBoost: '游戏启动后，进程优先级调为“高于正常”' }, changedNow: '当前', settingsProblem: (name) => `游戏设置有误：${name}`,
  tagSparxie: 'Sparxie 应用',
  // every error_code the Sparxie routes answer with, as one plain sentence; a code that is not here shows the host's own message
  errors: {
    sparxie_running: '关闭 Sparxie 后才能写入它的配置', config_unreadable: 'Sparxie 的配置文件无法读取', config_conflict: 'Sparxie 的配置在写入期间发生了变化', schema_unsupported: '不支持这个版本的 Sparxie 配置',
    write_failed: '无法写入 Sparxie 配置', apps_write_failed: '无法保存应用列表', no_user_session: '这台电脑当前没有已登录的用户', not_found: '没有找到这个应用，应用列表可能已经变化',
    invalid_input: '填写的内容无效', invalid_region: '所选区服与安装位置不符', is_sparxie: '这是 Sparxie 的位置，不是游戏', wrong_game: '这个文件不属于所选游戏', unrecognised: '没有找到已知的游戏主程序',
    game_running: '游戏正在运行，关闭游戏后再保存', registry_write_failed: '游戏设置没有写入成功', ambiguous: '这个文件夹里有多个已知游戏的主程序', not_readable: '无法读取这个文件夹', settings_unsupported: '这个游戏的设置只能在游戏内修改', launch_failed: '无法启动 Sparxie', art_unavailable: '无法生成官方海报', absent: '尚未安装 Sparxie', launch_unavailable: '主机无法启动 Sparxie', picker_unavailable: '主机不提供文件选择窗口',
  },
  moreChips: '更多',
}
const EN = {
  games: { genshin: 'Genshin Impact', starrail: 'Honkai: Star Rail', zzz: 'Zenless Zone Zero' },
  regions: { cn: 'CN', bili: 'Bilibili', global: 'Global' },
  sources: { hoyoplay: 'HoYoPlay', 'official-launcher': 'Official launcher', bili: 'Bilibili', steam: 'Steam', epic: 'Epic', folder: 'Folder search', manual: 'Chosen manually' },
  addGame: 'Add game', importProfiles: 'Import from Sparxie',
  newTitle: 'Add game', importTitle: 'Import from Sparxie', create: 'Create', cancel: 'Cancel', retry: 'Check again',
  secGame: 'Game', secRegion: 'Region', secInstall: 'Install location', secMode: 'Launch mode', secNameCover: 'Name and cover',
  detectedTag: 'Detected', inSparxie: 'In Sparxie', sparxieOnly: 'In Sparxie',
  installNone: 'Not found automatically. Choose the executable or a folder.', barNoGame: 'No game', barNoRegion: 'No region', barNoInstall: 'No install location', needGame: 'Choose a game first', needRegion: 'Choose a region first', needInstall: 'Choose an install location first', writeFailed: 'The Sparxie config could not be written', installChoose: 'Choose an install', changeInstall: 'Change location', installFrom: 'From Sparxie', installFromGame: 'Launcher registry',
  pickExe: 'Choose the game executable', pickExeText: (names) => names, pickFolder: 'Choose the game folder', pickFolderText: 'Searched level by level from the first level of the folder',
  scanning: 'Searching…', scanNone: 'The game was not found in that folder', scanTimeout: 'The search took too long; choose a more specific folder', scanFound: (n) => `${n} installs found; choose one`,
  mismatchRegion: (found, chosen) => `That location is ${found}, not ${chosen}`,
  wrongGame: (names) => `That file is not the chosen game’s. Accepted names: ${names}`, isSparxie: 'That is Sparxie’s own location; choose where the game is',
  noUser: 'Nobody is signed in on this PC', pickFailed: 'The picker could not be opened', identifyFailed: 'That location could not be identified', createFailed: 'Create failed', saveFailed: 'Save failed', failedWith: (verb, reason) => `${verb}: ${reason}`,
  storeEmpty: 'Sparxie has no game profiles', storeUnused: 'Sparxie has not created its config file yet', writeBack: 'The Sparxie profile is written when the application is created', writeBackNew: 'The Sparxie config file is created and written with the application',
  writeBackNone: 'The game is already in Sparxie; its config is not changed',
  closeSparxie: 'Close Sparxie to add the game to its config', unreadable: 'Sparxie’s config file cannot be read',
  region: 'Region', coverAuto: 'Official poster, updated with each version', coverChange: 'Change cover',
  appName: 'Application name',
  browseTitleFile: 'Choose the game executable', browseTitleFolder: 'Choose the game folder', browseRoot: 'This PC', browseUp: 'Up', browseHere: 'Choose this folder', browseEmpty: 'Nothing to show here', browseMore: (n) => `Showing the first ${n}`, browseFailed: 'This location cannot be read', browseClose: 'Back',
  coverFollow: 'Follow the official poster', coverFollowText: 'Updates with each version', coverUpdated: (t) => `Updated ${t}`, coverNone: 'No official poster yet', coverCustom: 'Custom cover', coverRestore: 'Restore the official poster',
  posterBadge: 'Show the Sparxie mark on the poster', posterBadgeOfficial: 'Drawn in the top-right corner of the poster', posterBadgeCustom: 'Drawn in the top-right corner of your cover (a local PNG or JPEG)',
  importEmpty: 'Sparxie has no profiles', importTaken: 'Already an application', importCount: (n) => `Import ${n}`, importAction: 'Import selected', importFoot: 'One application is created per profile; the profile is kept',
  openComponent: 'Open Sparxie', createInSparxie: 'Create a profile in Sparxie', refresh: 'Refresh', openFailed: 'Sparxie could not be opened', alreadyOpen: 'Sparxie is already open',
  addLocal: 'Add local game',
  addApp: 'Add app', searchGames: 'Search games', clearSearch: 'Clear search', noTitle: (q) => `Nothing matches “${q}”`, save: 'Save', editTitle: 'Edit game',
  gameHint: (title, region) => `Recognised as ${title}${region ? ` · ${region}` : ''}. Add it as a game instead?`, gameHintAction: 'Add as a game',
  scanSource: { steam: 'Steam', epic: 'Epic', gog: 'GOG', xbox: 'Xbox', ea: 'EA', ubisoft: 'Ubisoft', battlenet: 'Battle.net', riot: 'Riot', hoyoplay: 'HoYoPlay', wegame: 'WeGame', sparxie: 'Sparxie', directory: 'Folder', other: 'Other' }, scanAdded: 'Added', scanGameApp: 'Game app', scanFromSparxie: 'Profiles from Sparxie',
  secGameSettings: 'Game settings', gsFpsHint: (n) => `The game itself tops out at ${n} fps. Choose a higher rate with Target frame rate in the Sparxie settings`, gsFollow: 'Follows the game', gsChanged: 'Changed', gsBack: 'Follow the game again', gsAllFollow: 'Follow the game for all', restoreNote: 'Only what this application wrote at launch is put back; an item the game changed while it ran keeps the game\'s value',
  mode: 'Launch mode', inputTouch: 'Touch', inputKbm: 'Keyboard, mouse and gamepad', inputName: 'Input', secInput: 'Input', secSparxie: 'Sparxie settings', pc: 'Direct', touchNeeds: 'Without the Sparxie component this application starts the game directly', kbmNeedsTouch: 'The installed Sparxie cannot keep the PC interface, so these Sparxie options do not apply to keyboard, mouse and gamepad and the game is started directly. Update Sparxie to use them', resolutionAuto: 'Follow the stream', gsNotManaged: (game) => `Changing ${game}\'s in-game settings is not supported yet`, src: { hoyoplay: 'HoYoPlay', steam: 'Steam', epic: 'Epic', bili: 'Bilibili client', sparxie: 'Sparxie', manual: 'Chosen by hand' }, modeTouchText: 'Starts through Sparxie; requires administrator rights', pcHint: 'Starts the game directly',
  path: 'Game path', version: 'Game version', change: 'Change',
  secTouch: 'Touch launch', secProfile: 'Profile', secFps: 'Frame rate', secUi: 'Touch interface', secAdv: 'Advanced',
  secDisplay: 'Display', displayFollow: 'Follow the stream', displayFixed: 'Fixed', displayFollowNow: (w, h) => `This stream runs fullscreen at ${w}×${h}`, displayFollowLater: 'Fullscreen at the resolution of the streaming device', displayFixedLine: (size, window) => `The game runs ${[size, window && window.toLowerCase()].filter(Boolean).join(', ')}`, secQuality: 'Graphics', secAudio: 'Audio', secLanguage: 'Language', secLaunch: 'Launch', gsRunning: 'The game is running; changes apply from the next launch', gsRunningSave: 'The game is running. Close it, then save', listSep: ', ', gsUnsupported: (names) => `This game version does not allow changing: ${names}`, gsApply: 'Apply these settings on launch', gsNow: 'Now', on: 'On', off: 'Off', fpsDisplay: 'Follow the display refresh rate', fpsBySparxie: 'Frame rate is set by Sparxie',
  resolution: 'Resolution', displayMode: 'Display mode', display: { fullscreen: 'Fullscreen', borderless: 'Borderless window', windowed: 'Windowed' }, frameRate: 'In-game frame rate limit', fpsOpt: (n) => (n === 0 ? 'Unlimited' : `${n}`),
  vsync: 'V-sync', renderScale: 'Render scale', preset: 'Graphics quality', presetOpts: { low: 'Low', medium: 'Medium', high: 'High', ultra: 'Highest' }, hdr: 'HDR',
  restoreAfter: 'When the application ends', restoreOpts: { restore: 'Restore previous settings', keep: 'Keep' }, resetDefaults: 'Restore game defaults',
  fpsUnlock: 'Frame rate unlock', fpsCap: 'Target frame rate', bgLimit: 'Limit background frame rate', bgFps: 'Background frame rate',
  scaleOverride: 'Override interface scale', scale: 'Interface scale', priority: 'Process priority', priorityOpts: ['Below normal', 'Normal', 'Above normal', 'High'], followPreset: 'Frame rate presets', p30: '30 preset', p60: '60 preset',
  profileName: 'Profile name', profileId: 'Profile ID', makeDefault: 'Set as default', deleteProfile: 'Delete profile', openSparxie: 'Open in Sparxie', syncNote: 'Settings are synchronised with Sparxie',
  noticeTitle: 'How launching through Sparxie works', notice: 'Sparxie injects code into the running game and changes its memory. This may break the game’s terms of service, and any risk to your account is yours.',
  noticeFoot: 'Sparxie injects code into the running game and changes its memory. This may break the game’s terms of service, and any risk to your account is yours.',
  // game settings editor
  native: 'native', gameResolution: 'In-game resolution', search: 'Search', searchNone: 'No matches', closeSheet: 'Close', outOfRange: (min, max) => `Range ${min}–${max}`,
  argsHint: 'Several arguments per line are fine. Only letters, digits and _ . / : = + - are allowed; an argument may sit in double quotes.', argsPlace: '-screen-quality 2',
  argsChar: (line, ch) => `Line ${line}: ${ch === ' ' ? 'a space inside quotes' : `“${ch}”`} is not allowed. The command line gives it a special meaning, so it could run another command`, argsQuote: (line) => `Line ${line}: a quote is not closed`,
  hints: { gameBoost: 'After the game starts, its process priority is set to Above normal' }, changedNow: 'Now', settingsProblem: (name) => `Game settings need a fix: ${name}`,
  tagSparxie: 'Sparxie application',
  errors: {
    sparxie_running: 'Close Sparxie before its config is written', config_unreadable: 'The Sparxie config file cannot be read', config_conflict: 'The Sparxie config changed while it was being written', schema_unsupported: 'This version of the Sparxie config is not supported',
    write_failed: 'The Sparxie config could not be written', apps_write_failed: 'The application list could not be saved', no_user_session: 'Nobody is signed in on this PC', not_found: 'That application was not found; the list may have changed',
    invalid_input: 'Some of the input is not valid', invalid_region: 'The region does not match the install', is_sparxie: 'That is Sparxie\'s own location, not a game', wrong_game: 'That file does not belong to the chosen game', unrecognised: 'No known game executable was found',
    game_running: 'The game is running. Close it, then save', registry_write_failed: 'The game settings could not be stored', ambiguous: 'That folder holds more than one known game executable', not_readable: 'That folder cannot be read', settings_unsupported: 'This game\'s settings can only be changed in the game', launch_failed: 'Sparxie could not be started', art_unavailable: 'The official poster could not be composed', absent: 'Sparxie is not installed', launch_unavailable: 'The host cannot start Sparxie', picker_unavailable: 'The host does not offer a file dialog',
  },
  moreChips: 'More',
}
export const COPY = Object.freeze({ zh: ZH, en: EN })
export const copyFor = (locale) => COPY[sparxieLang(locale)]

/** The line under a failed save or create: "保存失败：<reason>". The reason is the sentence for the host's error_code, else the host's own message; only when the host said nothing is it the bare verb. */
export function failureText(C, verb, error) {
  const code = error && error.code ? String(error.code) : ''
  const message = error && error.message ? String(error.message) : ''
  const reason = (code && C.errors[code]) || (message && message !== code && message !== 'failed' ? message : '')
  return reason ? C.failedWith(verb, reason) : verb
}
