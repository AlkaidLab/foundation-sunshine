/**
 * Steam API工具模块
 * 提供Steam应用搜索和封面获取功能
 *
 * 注意: Steam ISteamApps/GetAppList API 已被弃用，现使用 Steam Store 搜索 API
 */

/**
 * 搜索Steam应用 (使用Steam Store搜索API)
 * @param {string} searchName 搜索名称
 * @param {number} maxResults 最大结果数量
 * @returns {Promise<Array>} 匹配的Steam应用列表
 */
export async function searchSteamApps(searchName, maxResults = 20) {
  if (!searchName || searchName.trim().length === 0) {
    return []
  }

  try {
    const response = await fetch(
      `/steam-store/api/storesearch/?term=${encodeURIComponent(searchName)}&l=schinese&cc=CN`
    )

    if (!response.ok) {
      console.warn('Steam搜索API请求失败:', response.status)
      return []
    }

    const data = await response.json()

    if (!data.items || !Array.isArray(data.items)) {
      return []
    }

    // 转换为统一格式
    return data.items
      .filter((item) => item.type === 'app')
      .slice(0, maxResults)
      .map((item) => ({
        appid: item.id,
        name: item.name,
        tiny_image: item.tiny_image,
        platforms: item.platforms,
        price: item.price,
        metascore: item.metascore,
      }))
  } catch (error) {
    console.warn('Steam搜索失败:', error)
    return []
  }
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
  try {
    const response = await fetch(`/steam-store/api/appdetails?appids=${appId}&l=schinese`)
    const data = await response.json()

    const appData = data[appId]
    if (appData && appData.success && appData.data) {
      return appData.data
    }
    return null
  } catch (error) {
    console.warn(`无法获取Steam应用 ${appId} 的详情:`, error)
    return null
  }
}

/**
 * 搜索Steam应用封面
 * @param {string} name 应用名称
 * @param {number} maxResults 最大结果数量
 * @returns {Promise<Array>} 封面列表
 */
export async function searchSteamCovers(name, maxResults = 20) {
  if (!name) {
    return []
  }

  // 使用Steam Store搜索API获取匹配的应用
  const matches = await searchSteamApps(name, maxResults)

  if (matches.length === 0) {
    return []
  }

  // 并行获取应用详情
  const detailPromises = matches.map(async (app) => {
    const gameData = await getSteamAppDetails(app.appid)

    if (gameData) {
      const headerImage = gameData.header_image || gameData.capsule_image || gameData.capsule_imagev5
      // 获取最佳封面URL（优先使用 library 竖版封面）
      const saveUrl = await getBestCoverUrl(app.appid, headerImage)

      return {
        name: gameData.name,
        appid: app.appid,
        source: 'steam',
        url: headerImage, // 预览使用 header 图片
        saveUrl: saveUrl, // 保存使用最佳可用封面
        key: `steam_${app.appid}`,
        type: gameData.type || 'game',
        shortDescription: gameData.short_description || '',
        developers: gameData.developers || [],
        publishers: gameData.publishers || [],
        releaseDate: gameData.release_date || null,
      }
    }
    return null
  })

  const results = await Promise.all(detailPromises)
  return results.filter((item) => item && item.url)
}

/**
 * 获取Steam封面图片URL
 * @param {number} appId Steam应用ID
 * @param {string} type 封面类型 ('header' | 'capsule' | 'library' | 'library_2x')
 * @returns {string} 封面图片URL
 */
export function getSteamCoverUrl(appId, type = 'header') {
  // 使用新的 Steam CDN 地址
  const baseUrl = 'https://cdn.cloudflare.steamstatic.com/steam/apps'

  switch (type) {
    case 'header':
      return `${baseUrl}/${appId}/header.jpg`
    case 'capsule':
      return `${baseUrl}/${appId}/capsule_231x87.jpg`
    case 'library':
      return `${baseUrl}/${appId}/library_600x900.jpg`
    case 'library_2x':
      return `${baseUrl}/${appId}/library_600x900_2x.jpg`
    default:
      return `${baseUrl}/${appId}/header.jpg`
  }
}

/**
 * 检查图片URL是否有效
 * @param {string} url 图片URL
 * @returns {Promise<boolean>} 是否有效
 */
export async function checkImageExists(url) {
  try {
    const response = await fetch(url, { method: 'HEAD' })
    return response.ok
  } catch {
    return false
  }
}

/**
 * 获取最佳可用的Steam封面URL
 * 优先返回 library 竖版封面，如果不存在则返回 header 横版封面
 * @param {number} appId Steam应用ID
 * @param {string} headerImage header图片URL (从API获取的)
 * @returns {Promise<string>} 最佳封面URL
 */
export async function getBestCoverUrl(appId, headerImage) {
  const libraryUrl = getSteamCoverUrl(appId, 'library')

  // 检查 library 封面是否存在
  const libraryExists = await checkImageExists(libraryUrl)

  if (libraryExists) {
    return libraryUrl
  }

  // 如果 library 封面不存在，使用 header 图片
  return headerImage || getSteamCoverUrl(appId, 'header')
}

/**
 * 验证Steam应用ID
 * @param {number|string} appId 应用ID
 * @returns {boolean} 是否有效
 */
export function isValidSteamAppId(appId) {
  const id = parseInt(appId)
  return !isNaN(id) && id > 0 && id < 2147483647 // 32位整数最大值
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

export default {
  loadSteamApps,
  searchSteamApps,
  getSteamAppDetails,
  searchSteamCovers,
  getSteamCoverUrl,
  checkImageExists,
  getBestCoverUrl,
  isValidSteamAppId,
  formatSteamAppInfo,
}
