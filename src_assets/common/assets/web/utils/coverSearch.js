/**
 * 封面搜索工具模块
 * 提供统一的IGDB和Steam封面搜索功能
 */

import { searchSteamCovers } from './steamApi.js'

// 共享缓存（模块级别，避免重复创建）
const bucketCache = new Map()
const gameCache = new Map()

// IGDB 相关常量
const IGDB_BASE_URL = 'https://lizardbyte.github.io/GameDB'
const IGDB_IMAGE_URL = 'https://images.igdb.com/igdb/image/upload/t_cover_big_2x'

/**
 * 获取搜索bucket（用于IGDB搜索）
 * @param {string} name 应用名称
 * @returns {string} bucket标识符
 */
export function getSearchBucket(name) {
  const bucket = name
    .substring(0, 2)
    .toLowerCase()
    .replace(/[^a-z\d\u4e00-\u9fa5]/g, '')
  return bucket || '@'
}

/**
 * 带缓存的fetch函数
 * @param {Map} cache 缓存Map
 * @param {string} key 缓存键
 * @param {Function} fetchFn 获取数据的函数
 * @returns {Promise<any>} 数据
 */
async function fetchWithCache(cache, key, fetchFn) {
  if (cache.has(key)) return cache.get(key)
  const data = await fetchFn()
  cache.set(key, data)
  return data
}

/**
 * 搜索IGDB封面（单个结果，返回URL字符串）
 * @param {string} searchName 搜索名称（已处理格式）
 * @param {string} bucket bucket标识符
 * @returns {Promise<string>} 封面URL，未找到返回空字符串
 */
export async function searchIGDBCover(searchName, bucket) {
  try {
    const maps = await fetchWithCache(bucketCache, bucket, async () => {
      const response = await fetch(`${IGDB_BASE_URL}/buckets/${bucket}.json`)
      return response.ok ? response.json() : null
    })

    if (!maps) return ''

    const matchedId = Object.keys(maps).find((id) =>
      maps[id].name.replace(/\s+/g, '.').toLowerCase().startsWith(searchName)
    )

    if (!matchedId) return ''

    const game = await fetchWithCache(gameCache, matchedId, async () => {
      const res = await fetch(`${IGDB_BASE_URL}/games/${matchedId}.json`)
      return res.ok ? res.json() : null
    })

    if (!game?.cover?.url) return ''

    const thumb = game.cover.url
    const hash = thumb.substring(thumb.lastIndexOf('/') + 1, thumb.lastIndexOf('.'))
    return `${IGDB_IMAGE_URL}/${hash}.png`
  } catch (error) {
    console.warn(`搜索IGDB封面失败: ${searchName}`, error)
    return ''
  }
}

/**
 * 搜索IGDB封面（多个结果，返回数组）
 * @param {string} name 应用名称
 * @param {AbortSignal} signal 可选的AbortSignal用于取消请求
 * @param {number} maxResults 最大结果数量
 * @returns {Promise<Array>} 封面结果数组
 */
export async function searchIGDBCovers(name, signal = null, maxResults = 20) {
  if (!name) return []

  const searchName = name.replace(/\s+/g, '.').toLowerCase()
  const bucket = getSearchBucket(name)

  try {
    // 使用缓存的bucket数据
    let maps = bucketCache.get(bucket)
    if (!maps) {
      const response = await fetch(`${IGDB_BASE_URL}/buckets/${bucket}.json`, { signal })
      if (!response.ok) throw new Error('Failed to search covers')
      maps = await response.json()
      bucketCache.set(bucket, maps)
    }

    const matchedIds = Object.keys(maps)
      .filter((id) => maps[id].name.replace(/\s+/g, '.').toLowerCase().startsWith(searchName))
      .slice(0, maxResults)

    // 并行获取游戏详情，使用缓存
    const games = await Promise.all(
      matchedIds.map(async (id) => {
        // 检查缓存
        if (gameCache.has(id)) {
          return gameCache.get(id)
        }
        try {
          const res = await fetch(`${IGDB_BASE_URL}/games/${id}.json`, { signal })
          const game = await res.json()
          gameCache.set(id, game)
          return game
        } catch {
          return null
        }
      })
    )

    return games
      .filter((game) => game?.cover?.url)
      .map((game) => {
        const thumb = game.cover.url
        const hash = thumb.substring(thumb.lastIndexOf('/') + 1, thumb.lastIndexOf('.'))
        return {
          name: game.name,
          key: `igdb_${game.id}`,
          source: 'igdb',
          url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${hash}.jpg`,
          saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${hash}.png`,
        }
      })
  } catch (error) {
    if (error.name === 'AbortError') {
      throw error
    }
    console.error('搜索IGDB封面失败:', error)
    return []
  }
}

/**
 * 搜索封面图片（单个结果，用于useApps.js）
 * 同时搜索IGDB和Steam，返回第一个找到的结果
 * @param {string} appName 应用名称
 * @returns {Promise<string>} 封面URL，未找到返回空字符串
 */
export async function searchCoverImage(appName) {
  if (!appName) return ''

  const searchName = appName.replace(/\s+/g, '.').toLowerCase()
  const bucket = getSearchBucket(appName)

  try {
    const [igdbResult, steamResult] = await Promise.allSettled([
      searchIGDBCover(searchName, bucket),
      searchSteamCovers(appName, 1).then((results) => (results.length > 0 ? results[0].saveUrl : '')),
    ])

    return (
      (igdbResult.status === 'fulfilled' && igdbResult.value) ||
      (steamResult.status === 'fulfilled' && steamResult.value) ||
      ''
    )
  } catch (error) {
    console.warn(`搜索封面失败: ${appName}`, error)
    return ''
  }
}

/**
 * 批量搜索封面图片
 * @param {Array} appList 应用列表
 * @returns {Promise<Array>} 带封面URL的应用列表
 */
export async function batchSearchCoverImages(appList) {
  const results = await Promise.allSettled(
    appList.map(async (app) => ({
      ...app,
      'image-path': await searchCoverImage(encodeURIComponent(app.name)),
    }))
  )
  return results.map((result, index) =>
    result.status === 'fulfilled' ? result.value : appList[index]
  )
}

/**
 * 同时搜索IGDB和Steam封面（多个结果，用于CoverFinder.vue）
 * @param {string} name 应用名称
 * @param {AbortSignal} signal 可选的AbortSignal用于取消请求
 * @returns {Promise<{igdb: Array, steam: Array}>} 包含IGDB和Steam结果的对象
 */
export async function searchAllCovers(name, signal = null) {
  if (!name) {
    return { igdb: [], steam: [] }
  }

  try {
    const [igdbResults, steamResults] = await Promise.allSettled([
      searchIGDBCovers(name, signal),
      searchSteamCovers(name),
    ])

    return {
      igdb: igdbResults.status === 'fulfilled' ? igdbResults.value : [],
      steam: steamResults.status === 'fulfilled' ? steamResults.value : [],
    }
  } catch (error) {
    if (error.name === 'AbortError') {
      throw error
    }
    console.error('搜索封面失败:', error)
    return { igdb: [], steam: [] }
  }
}

/**
 * 清除缓存
 */
export function clearCache() {
  bucketCache.clear()
  gameCache.clear()
}
