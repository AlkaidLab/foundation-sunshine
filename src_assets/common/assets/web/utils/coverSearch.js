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

// 缓存 Tauri 环境检测结果
let _isTauriEnv = null

/**
 * 检测是否在 Tauri 环境中
 * @returns {boolean} 是否在 Tauri 环境
 */
function isTauriEnv() {
  if (_isTauriEnv === null) {
    _isTauriEnv = typeof window !== 'undefined' && !!(window.isTauri || window.__TAURI__)
  }
  return _isTauriEnv
}

/**
 * 构建代理 URL（用于绕过 CORS 限制）
 * @param {string} url 原始 URL
 * @returns {string} 代理 URL 或原始 URL
 */
function buildProxyUrl(url) {
  return isTauriEnv() ? `/_proxy/?url=${encodeURIComponent(url)}` : url
}

/**
 * 获取搜索bucket（用于IGDB搜索）
 * 注意：IGDB bucket 只支持英文字母和数字，中文等非ASCII字符会返回 '@'
 * @param {string} name 应用名称
 * @returns {string} bucket标识符
 */
export function getSearchBucket(name) {
  const bucket = name
    .substring(0, 2)
    .toLowerCase()
    .replace(/[^a-z\d]/g, '')
  return bucket || '@'
}

/**
 * 检查搜索词是否适合IGDB搜索
 * IGDB的bucket系统只支持英文，中文等非ASCII字符无法正确匹配
 * @param {string} name 搜索名称
 * @returns {boolean} 是否适合IGDB搜索
 */
function isValidForIGDB(name) {
  if (!name) return false
  // 检查是否包含至少一个英文字母或数字
  return /[a-zA-Z\d]/.test(name)
}

// 预编译正则表达式
const SEPARATOR_REGEX = /[:\-_''""]/g
const WHITESPACE_REGEX = /\s+/g

/**
 * 规范化搜索字符串
 * @param {string} str 原始字符串
 * @returns {string} 规范化后的字符串
 */
function normalizeSearchString(str) {
  return str.toLowerCase().replace(SEPARATOR_REGEX, ' ').replace(WHITESPACE_REGEX, ' ').trim()
}

/**
 * 生成字符串的bigrams集合
 * @param {string} str 输入字符串
 * @returns {Set<string>} bigrams集合
 */
function getBigrams(str) {
  const bigrams = new Set()
  const len = str.length - 1
  for (let i = 0; i < len; i++) {
    bigrams.add(str.substring(i, i + 2))
  }
  return bigrams
}

/**
 * 计算字符串相似度（Dice系数）
 * @param {string} str1 字符串1
 * @param {string} str2 字符串2
 * @returns {number} 相似度 0-1
 */
function calculateSimilarity(str1, str2) {
  const s1 = normalizeSearchString(str1)
  const s2 = normalizeSearchString(str2)

  if (s1 === s2) return 1
  if (s1.length < 2 || s2.length < 2) return 0

  const bigrams1 = getBigrams(s1)
  const bigrams2 = getBigrams(s2)

  let intersection = 0
  for (const bigram of bigrams1) {
    if (bigrams2.has(bigram)) intersection++
  }

  return (2 * intersection) / (bigrams1.size + bigrams2.size)
}

/**
 * 检查是否匹配搜索词
 * @param {string} gameName 游戏名称
 * @param {string} searchTerm 搜索词
 * @returns {{match: boolean, score: number}} 匹配结果和分数
 */
function matchesSearch(gameName, searchTerm) {
  const normalizedGame = normalizeSearchString(gameName)
  const normalizedSearch = normalizeSearchString(searchTerm)

  // 完全匹配
  if (normalizedGame === normalizedSearch) {
    return { match: true, score: 1 }
  }

  // 前缀匹配（高优先级）
  if (normalizedGame.startsWith(normalizedSearch)) {
    return { match: true, score: 0.95 }
  }

  // 包含匹配
  if (normalizedGame.includes(normalizedSearch)) {
    return { match: true, score: 0.85 }
  }

  // 单词匹配（搜索词的所有单词都在游戏名中）
  const searchWords = normalizedSearch.split(' ').filter((w) => w.length > 1)
  if (searchWords.length > 0) {
    const gameWords = normalizedGame.split(' ')
    const allWordsMatch = searchWords.every((sw) => gameWords.some((gw) => gw.startsWith(sw) || gw.includes(sw)))
    if (allWordsMatch) {
      return { match: true, score: 0.8 }
    }
  }

  // 相似度匹配
  const similarity = calculateSimilarity(gameName, searchTerm)
  if (similarity > 0.5) {
    return { match: true, score: similarity * 0.7 }
  }

  return { match: false, score: 0 }
}

/**
 * 带缓存的fetch函数
 * @param {Map} cache 缓存Map
 * @param {string} key 缓存键
 * @param {Function} fetchFn 获取数据的函数
 * @returns {Promise<any>} 数据
 */
async function fetchWithCache(cache, key, fetchFn) {
  const cached = cache.get(key)
  if (cached !== undefined) return cached
  const data = await fetchFn()
  cache.set(key, data)
  return data
}

/**
 * 从封面URL提取hash并构建完整URL
 * @param {string} thumbUrl 缩略图URL
 * @param {string} size 图片尺寸
 * @param {string} ext 文件扩展名
 * @returns {string} 完整的图片URL
 */
function buildIGDBImageUrl(thumbUrl, size = 't_cover_big_2x', ext = 'png') {
  const lastSlash = thumbUrl.lastIndexOf('/')
  const lastDot = thumbUrl.lastIndexOf('.')
  const hash = thumbUrl.substring(lastSlash + 1, lastDot)
  return `https://images.igdb.com/igdb/image/upload/${size}/${hash}.${ext}`
}

/**
 * 搜索IGDB封面（单个结果，返回URL字符串）
 * @param {string} searchName 搜索名称
 * @param {string} bucket bucket标识符
 * @returns {Promise<string>} 封面URL，未找到返回空字符串
 */
export async function searchIGDBCover(searchName, bucket) {
  // 检查搜索词是否适合IGDB搜索
  if (!isValidForIGDB(searchName)) {
    return ''
  }

  try {
    const maps = await fetchWithCache(bucketCache, bucket, async () => {
      const url = `${IGDB_BASE_URL}/buckets/${bucket}.json`
      const response = await fetch(buildProxyUrl(url))
      return response.ok ? response.json() : null
    })

    if (!maps) return ''

    let bestMatch = null
    let bestScore = 0

    const ids = Object.keys(maps)
    for (let i = 0; i < ids.length; i++) {
      const id = ids[i]
      const { match, score } = matchesSearch(maps[id].name, searchName)
      if (match && score > bestScore) {
        bestScore = score
        bestMatch = id
      }
    }

    if (!bestMatch) return ''

    const game = await fetchWithCache(gameCache, bestMatch, async () => {
      const url = `${IGDB_BASE_URL}/games/${bestMatch}.json`
      const res = await fetch(buildProxyUrl(url))
      return res.ok ? res.json() : null
    })

    if (!game?.cover?.url) return ''

    return buildIGDBImageUrl(game.cover.url)
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

  // 检查搜索词是否适合IGDB搜索（IGDB只支持英文搜索）
  if (!isValidForIGDB(name)) {
    console.debug(`IGDB搜索跳过：搜索词 "${name}" 不包含英文字符`)
    return []
  }

  const bucket = getSearchBucket(name)

  try {
    const maps = await fetchWithCache(bucketCache, bucket, async () => {
      const url = `${IGDB_BASE_URL}/buckets/${bucket}.json`
      const response = await fetch(buildProxyUrl(url), { signal })
      if (!response.ok) {
        // 404 表示该 bucket 不存在，这是正常情况，返回空对象
        if (response.status === 404) {
          return {}
        }
        throw new Error('Failed to search covers')
      }
      return response.json()
    })

    // 使用改进的匹配算法，收集所有匹配项并按分数排序
    const matches = []
    const ids = Object.keys(maps)
    for (let i = 0; i < ids.length; i++) {
      const id = ids[i]
      const { match, score } = matchesSearch(maps[id].name, name)
      if (match) {
        matches.push({ id, score, name: maps[id].name })
      }
    }

    // 按分数降序排序，取前maxResults个
    matches.sort((a, b) => b.score - a.score)
    const matchedIds = matches.slice(0, maxResults).map((m) => m.id)

    // 并行获取游戏详情，使用缓存
    const games = await Promise.all(
      matchedIds.map(async (id) => {
        return fetchWithCache(gameCache, id, async () => {
          try {
            const url = `${IGDB_BASE_URL}/games/${id}.json`
            const res = await fetch(buildProxyUrl(url), { signal })
            return res.json()
          } catch {
            return null
          }
        })
      })
    )

    const results = []
    for (let i = 0; i < games.length; i++) {
      const game = games[i]
      if (game?.cover?.url) {
        const thumb = game.cover.url
        results.push({
          name: game.name,
          key: `igdb_${game.id}`,
          source: 'igdb',
          url: buildIGDBImageUrl(thumb, 't_cover_big', 'jpg'),
          saveUrl: buildIGDBImageUrl(thumb),
        })
      }
    }
    return results
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

  const bucket = getSearchBucket(appName)

  try {
    const [igdbResult, steamResult] = await Promise.allSettled([
      searchIGDBCover(appName, bucket),
      searchSteamCovers(appName, 1).then((results) => results[0]?.saveUrl || ''),
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
  return results.map((result, index) => (result.status === 'fulfilled' ? result.value : appList[index]))
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
