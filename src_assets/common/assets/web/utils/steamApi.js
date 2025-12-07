/**
 * Steam API工具模块
 * 提供Steam应用搜索和封面获取功能
 *
 */

// Steam CDN 基础URL
const STEAM_CDN_BASE = 'https://cdn.cloudflare.steamstatic.com/steam/apps'

// SteamGridDB API 基础URL
const STEAMGRIDDB_API_BASE = '/steamgriddb'

// 封面URL缓存
const coverUrlCache = new Map()

// SteamGridDB 缓存
const steamGridDBCache = new Map()

// 图片存在性缓存
const imageExistsCache = new Map()

/**
 * 构建URL查询参数
 * @param {Object} options 参数对象
 * @returns {string} 查询字符串
 */
function buildQueryString(options) {
  const params = new URLSearchParams()
  Object.entries(options).forEach(([key, value]) => {
    if (value !== undefined && value !== null) {
      params.append(key, value)
    }
  })
  const str = params.toString()
  return str ? `?${str}` : ''
}

/**
 * 通用fetch请求封装
 * @param {string} url 请求URL
 * @param {Object} options fetch选项
 * @returns {Promise<Object|null>} 响应数据
 */
async function fetchJson(url, options = {}) {
  try {
    const response = await fetch(url, options)
    if (!response.ok) {
      return null
    }
    return await response.json()
  } catch {
    return null
  }
}

/**
 * 搜索Steam应用 (使用Steam Store搜索API)
 * @param {string} searchName 搜索名称
 * @param {number} maxResults 最大结果数量
 * @returns {Promise<Array>} 匹配的Steam应用列表
 */
export async function searchSteamApps(searchName, maxResults = 20) {
  if (!searchName?.trim()) {
    return []
  }

  const data = await fetchJson(`/steam-store/api/storesearch/?term=${encodeURIComponent(searchName)}&l=schinese&cc=CN`)

  if (!data?.items?.length) {
    return []
  }

  return data.items
    .filter((item) => item.type === 'app')
    .slice(0, maxResults)
    .map(({ id, name, tiny_image, platforms, price, metascore }) => ({
      appid: id,
      name,
      tiny_image,
      platforms,
      price,
      metascore,
    }))
}

/**
 * 加载Steam应用列表 (已弃用，保留兼容性)
 * @deprecated 使用 searchSteamApps 代替
 * @returns {Promise<Array>} 空数组
 */
export async function loadSteamApps() {
  console.warn('loadSteamApps 已弃用，请使用 searchSteamApps 直接搜索')
  return []
}

/**
 * 获取Steam应用详情
 * @param {number} appId Steam应用ID
 * @returns {Promise<Object|null>} Steam应用详情
 */
export async function getSteamAppDetails(appId) {
  const data = await fetchJson(`/steam-store/api/appdetails?appids=${appId}&l=schinese`)
  return data?.[appId]?.success ? data[appId].data : null
}

/**
 * 搜索Steam应用封面（快速模式）
 * 优化：直接使用CDN URL，不再获取详情API，大幅提升速度
 * @param {string} name 应用名称
 * @param {number} maxResults 最大结果数量
 * @returns {Promise<Array>} 封面列表
 */
export async function searchSteamCovers(name, maxResults = 20) {
  if (!name) {
    return []
  }

  const matches = await searchSteamApps(name, maxResults)

  if (!matches.length) {
    return []
  }

  const coverPromises = matches.map(async ({ appid, name: appName }) => ({
    name: appName,
    appid,
    source: 'steam',
    url: getSteamCoverUrl(appid, 'header'),
    saveUrl: await getCachedBestCoverUrl(appid),
    key: `steam_${appid}`,
  }))

  return Promise.all(coverPromises)
}

/**
 * 搜索Steam应用封面（完整模式，包含详情）
 * @param {string} name 应用名称
 * @param {number} maxResults 最大结果数量
 * @returns {Promise<Array>} 封面列表（包含详情）
 */
export async function searchSteamCoversWithDetails(name, maxResults = 20) {
  if (!name) {
    return []
  }

  const matches = await searchSteamApps(name, maxResults)

  if (!matches.length) {
    return []
  }

  const detailPromises = matches.map(async ({ appid }) => {
    const gameData = await getSteamAppDetails(appid)

    if (!gameData) {
      return null
    }

    const headerImage = gameData.header_image || gameData.capsule_image || gameData.capsule_imagev5
    const saveUrl = await getCachedBestCoverUrl(appid)

    return {
      name: gameData.name,
      appid,
      source: 'steam',
      url: headerImage,
      saveUrl,
      key: `steam_${appid}`,
      type: gameData.type || 'game',
      shortDescription: gameData.short_description || '',
      developers: gameData.developers || [],
      publishers: gameData.publishers || [],
      releaseDate: gameData.release_date || null,
    }
  })

  const results = await Promise.all(detailPromises)
  return results.filter((item) => item?.url)
}

// 封面类型映射表
const COVER_TYPE_MAP = {
  header: 'header.jpg',
  header_292x136: 'header_292x136.jpg',
  capsule: 'capsule_231x87.jpg',
  capsule_231x87: 'capsule_231x87.jpg',
  capsule_616x353: 'capsule_616x353.jpg',
  library: 'library_600x900.jpg',
  library_600x900: 'library_600x900.jpg',
  library_2x: 'library_600x900_2x.jpg',
  library_600x900_2x: 'library_600x900_2x.jpg',
  library_hero: 'library_hero.jpg',
  library_hero_2x: 'library_hero_2x.jpg',
  logo: 'logo.png',
  page_bg: 'page_bg_generated_v6b.jpg',
}

/**
 * 获取Steam封面图片URL
 * @param {number} appId Steam应用ID
 * @param {string} type 封面类型
 * @returns {string} 封面图片URL
 */
export function getSteamCoverUrl(appId, type = 'header') {
  const filename = COVER_TYPE_MAP[type] || COVER_TYPE_MAP.header
  return `${STEAM_CDN_BASE}/${appId}/${filename}`
}

/**
 * 检查图片URL是否有效（带缓存）
 * @param {string} url 图片URL
 * @returns {Promise<boolean>} 是否有效
 */
export async function checkImageExists(url) {
  if (imageExistsCache.has(url)) {
    return imageExistsCache.get(url)
  }

  try {
    const response = await fetch(url, { method: 'HEAD' })
    const exists = response.ok
    imageExistsCache.set(url, exists)
    return exists
  } catch {
    imageExistsCache.set(url, false)
    return false
  }
}

/**
 * 获取最佳可用的Steam封面URL（带缓存）
 * @param {number} appId Steam应用ID
 * @returns {Promise<string>} 最佳封面URL
 */
export async function getCachedBestCoverUrl(appId) {
  if (coverUrlCache.has(appId)) {
    return coverUrlCache.get(appId)
  }

  const libraryUrl = getSteamCoverUrl(appId, 'library')
  const libraryExists = await checkImageExists(libraryUrl)
  const bestUrl = libraryExists ? libraryUrl : getSteamCoverUrl(appId, 'header')

  coverUrlCache.set(appId, bestUrl)
  return bestUrl
}

/**
 * 获取最佳可用的Steam封面URL
 * @param {number} appId Steam应用ID
 * @param {string} headerImage header图片URL (从API获取的)
 * @returns {Promise<string>} 最佳封面URL
 */
export async function getBestCoverUrl(appId, headerImage) {
  const libraryUrl = getSteamCoverUrl(appId, 'library')
  const libraryExists = await checkImageExists(libraryUrl)
  return libraryExists ? libraryUrl : headerImage || getSteamCoverUrl(appId, 'header')
}

/**
 * 批量获取Steam封面URL（优化版本）
 * @param {Array<number>} appIds Steam应用ID数组
 * @returns {Promise<Map<number, string>>} appId到封面URL的映射
 */
export async function batchGetCoverUrls(appIds) {
  const results = new Map()
  const uncachedIds = []

  for (const appId of appIds) {
    if (coverUrlCache.has(appId)) {
      results.set(appId, coverUrlCache.get(appId))
    } else {
      uncachedIds.push(appId)
    }
  }

  if (uncachedIds.length > 0) {
    const fetched = await Promise.all(
      uncachedIds.map(async (appId) => ({
        appId,
        url: await getCachedBestCoverUrl(appId),
      }))
    )
    fetched.forEach(({ appId, url }) => results.set(appId, url))
  }

  return results
}

/**
 * 清除封面URL缓存
 */
export function clearCoverCache() {
  coverUrlCache.clear()
  steamGridDBCache.clear()
  imageExistsCache.clear()
}

/**
 * 验证Steam应用ID
 * @param {number|string} appId 应用ID
 * @returns {boolean} 是否有效
 */
export function isValidSteamAppId(appId) {
  const id = parseInt(appId)
  return !isNaN(id) && id > 0 && id < 2147483647
}

/**
 * 格式化Steam应用信息
 * @param {Object} appData Steam应用数据
 * @returns {Object} 格式化后的应用信息
 */
export function formatSteamAppInfo(appData) {
  return {
    id: appData.steam_appid,
    name: appData.name,
    type: appData.type,
    description: appData.short_description,
    developers: appData.developers || [],
    publishers: appData.publishers || [],
    releaseDate: appData.release_date?.date || null,
    price: appData.price_overview || null,
    categories: appData.categories || [],
    genres: appData.genres || [],
    screenshots: appData.screenshots || [],
    movies: appData.movies || [],
    achievements: appData.achievements || [],
    platforms: appData.platforms || {},
    metacritic: appData.metacritic || null,
    recommendations: appData.recommendations || null,
  }
}

// ==================== SteamGridDB 支持 ====================

/**
 * 通用SteamGridDB资源映射函数
 * @param {Object} item 资源项
 * @returns {Object} 映射后的对象
 */
function mapSteamGridDBItem(item) {
  return {
    id: item.id,
    url: item.url,
    thumb: item.thumb,
    width: item.width,
    height: item.height,
    style: item.style,
    nsfw: item.nsfw,
    humor: item.humor,
    author: item.author,
    language: item.language,
    score: item.score,
    ...(item.lock !== undefined && { lock: item.lock }),
    ...(item.epilepsy !== undefined && { epilepsy: item.epilepsy }),
  }
}

/**
 * 通用SteamGridDB资源获取函数
 * @param {string} resourceType 资源类型 (grids, heroes, logos, icons)
 * @param {number} gameId 游戏ID
 * @param {Object} options 选项
 * @returns {Promise<Array>} 资源列表
 */
async function fetchSteamGridDBResource(resourceType, gameId, options = {}) {
  if (!gameId) {
    return []
  }

  const queryString = buildQueryString(options)
  const url = `${STEAMGRIDDB_API_BASE}/${resourceType}/game/${gameId}${queryString}`
  const data = await fetchJson(url)

  if (!data?.success || !data?.data) {
    return []
  }

  return data.data.map(mapSteamGridDBItem)
}

/**
 * 在SteamGridDB上搜索游戏
 * @param {string} searchTerm 搜索词
 * @returns {Promise<Array>} 游戏列表
 */
export async function searchSteamGridDB(searchTerm) {
  if (!searchTerm?.trim()) {
    return []
  }

  const data = await fetchJson(`${STEAMGRIDDB_API_BASE}/search/autocomplete/${encodeURIComponent(searchTerm)}`)

  if (!data?.success || !data?.data) {
    return []
  }

  return data.data.map((game) => ({
    id: game.id,
    name: game.name,
    releaseDate: game.release_date,
    types: game.types || [],
    verified: game.verified || false,
  }))
}

/**
 * 通过Steam AppID获取SteamGridDB游戏ID
 * @param {number} steamAppId Steam应用ID
 * @returns {Promise<number|null>} SteamGridDB游戏ID
 */
export async function getSteamGridDBGameId(steamAppId) {
  const cacheKey = `steam_${steamAppId}`
  if (steamGridDBCache.has(cacheKey)) {
    return steamGridDBCache.get(cacheKey)
  }

  const data = await fetchJson(`${STEAMGRIDDB_API_BASE}/games/steam/${steamAppId}`)

  if (data?.success && data?.data) {
    const gameId = data.data.id
    steamGridDBCache.set(cacheKey, gameId)
    return gameId
  }

  return null
}

/**
 * 获取SteamGridDB封面（Grids）
 * @param {number} gameId SteamGridDB游戏ID
 * @param {Object} options 选项
 * @returns {Promise<Array>} 封面列表
 */
export function getSteamGridDBGrids(gameId, options = {}) {
  return fetchSteamGridDBResource('grids', gameId, options)
}

/**
 * 获取SteamGridDB英雄图（Heroes）
 * @param {number} gameId SteamGridDB游戏ID
 * @param {Object} options 选项
 * @returns {Promise<Array>} 英雄图列表
 */
export function getSteamGridDBHeroes(gameId, options = {}) {
  return fetchSteamGridDBResource('heroes', gameId, options)
}

/**
 * 获取SteamGridDB Logo
 * @param {number} gameId SteamGridDB游戏ID
 * @param {Object} options 选项
 * @returns {Promise<Array>} Logo列表
 */
export function getSteamGridDBLogos(gameId, options = {}) {
  return fetchSteamGridDBResource('logos', gameId, options)
}

/**
 * 获取SteamGridDB图标（Icons）
 * @param {number} gameId SteamGridDB游戏ID
 * @param {Object} options 选项
 * @returns {Promise<Array>} 图标列表
 */
export function getSteamGridDBIcons(gameId, options = {}) {
  return fetchSteamGridDBResource('icons', gameId, options)
}

// 默认SteamGridDB选项
const DEFAULT_GRID_OPTIONS = {
  dimensions: '600x900',
  types: 'static',
  nsfw: 'false',
  humor: 'false',
}

/**
 * 搜索SteamGridDB封面（综合搜索）
 * @param {string} name 游戏名称
 * @param {number} maxResults 最大结果数量
 * @param {Object} gridOptions 封面选项
 * @returns {Promise<Array>} 封面列表
 */
export async function searchSteamGridDBCovers(name, maxResults = 20, gridOptions = {}) {
  if (!name) {
    return []
  }

  const games = await searchSteamGridDB(name)

  if (!games.length) {
    return []
  }

  const options = { ...DEFAULT_GRID_OPTIONS, ...gridOptions }

  const coverPromises = games.slice(0, 10).map(async (game) => {
    const grids = await getSteamGridDBGrids(game.id, options)

    return grids.slice(0, 3).map((grid) => ({
      name: game.name,
      gameId: game.id,
      source: 'steamgriddb',
      url: grid.thumb || grid.url,
      saveUrl: grid.url,
      key: `sgdb_${game.id}_${grid.id}`,
      width: grid.width,
      height: grid.height,
      style: grid.style,
      author: grid.author,
      score: grid.score,
    }))
  })

  const results = await Promise.all(coverPromises)
  return results.flat().slice(0, maxResults)
}

/**
 * 通过Steam AppID获取SteamGridDB封面
 * @param {number} steamAppId Steam应用ID
 * @param {Object} gridOptions 封面选项
 * @returns {Promise<Array>} 封面列表
 */
export async function getSteamGridDBCoversBySteamId(steamAppId, gridOptions = {}) {
  const gameId = await getSteamGridDBGameId(steamAppId)

  if (!gameId) {
    return []
  }

  const options = { ...DEFAULT_GRID_OPTIONS, ...gridOptions }
  const grids = await getSteamGridDBGrids(gameId, options)

  return grids.map((grid) => ({
    gameId,
    steamAppId,
    source: 'steamgriddb',
    url: grid.thumb || grid.url,
    saveUrl: grid.url,
    key: `sgdb_${gameId}_${grid.id}`,
    width: grid.width,
    height: grid.height,
    style: grid.style,
    author: grid.author,
    score: grid.score,
  }))
}

export default {
  loadSteamApps,
  searchSteamApps,
  getSteamAppDetails,
  searchSteamCovers,
  searchSteamCoversWithDetails,
  getSteamCoverUrl,
  checkImageExists,
  getBestCoverUrl,
  getCachedBestCoverUrl,
  batchGetCoverUrls,
  clearCoverCache,
  isValidSteamAppId,
  formatSteamAppInfo,
  // SteamGridDB
  searchSteamGridDB,
  getSteamGridDBGameId,
  getSteamGridDBGrids,
  getSteamGridDBHeroes,
  getSteamGridDBLogos,
  getSteamGridDBIcons,
  searchSteamGridDBCovers,
  getSteamGridDBCoversBySteamId,
}
