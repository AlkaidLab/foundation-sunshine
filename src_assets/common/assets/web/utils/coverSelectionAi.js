import { searchAllCovers } from './coverSearch.js'
import { API_ENDPOINTS } from './constants.js'
import { getCoverSearchCandidates } from './gameMetadataAi.js'
import { buildLocalizedInstruction, getCurrentLocale, getPromptLanguageName } from './aiLocale.js'
import { createAiCache } from './aiCache.js'

const MAX_SEARCH_TERMS = 3
const MAX_COVERS_PER_TERM = 6
const MAX_AI_CANDIDATES = 12
const AI_COVER_SELECTION_TIMEOUT_MS = 30000
const coverCache = createAiCache('cover-selection', { version: 'v1' })

function clamp01(value) {
  return Math.max(0, Math.min(1, Number(value) || 0))
}

function normalizeTitle(value) {
  return String(value || '')
    .toLowerCase()
    .replace(/[^a-z0-9\u4e00-\u9fff]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

function getCandidateNames(app) {
  return Array.from(new Set([
    app?.['canonical-name'],
    app?.name,
    app?.['original-name'],
    ...(Array.isArray(app?.['cover-search-terms']) ? app['cover-search-terms'] : []),
  ].map(normalizeTitle).filter(Boolean)))
}

function getTokenOverlap(first, second) {
  const firstTokens = new Set(first.split(' ').filter((token) => token.length > 1))
  const secondTokens = new Set(second.split(' ').filter((token) => token.length > 1))
  if (firstTokens.size === 0 || secondTokens.size === 0) return 0

  let shared = 0
  for (const token of firstTokens) {
    if (secondTokens.has(token)) shared += 1
  }
  return shared / Math.max(firstTokens.size, secondTokens.size)
}

export function assessCoverMatch(app, candidate) {
  const title = normalizeTitle(candidate?.name)
  const names = getCandidateNames(app)

  let best = {
    confidence: candidate?.source === 'igdb' ? 0.42 : 0.36,
    relation: 'source-prior',
    matchedName: '',
    reason: candidate?.source === 'igdb' ? 'IGDB source prior' : 'Steam source prior',
  }

  if (!title) {
    return best
  }

  for (const name of names) {
    let evidence = null
    if (!name) continue

    if (title === name) {
      evidence = { confidence: 0.96, relation: 'exact-title', reason: 'Exact title match' }
    } else if (title.startsWith(name) || name.startsWith(title)) {
      evidence = { confidence: 0.84, relation: 'prefix-title', reason: 'Prefix title match' }
    } else if (title.includes(name) || name.includes(title)) {
      evidence = { confidence: 0.74, relation: 'contains-title', reason: 'Partial title match' }
    } else {
      const overlap = getTokenOverlap(title, name)
      if (overlap >= 0.72) {
        evidence = { confidence: 0.66, relation: 'token-overlap', reason: 'Strong token overlap' }
      }
    }

    if (evidence && evidence.confidence > best.confidence) {
      best = {
        ...evidence,
        matchedName: name,
      }
    }
  }

  const searchTerm = normalizeTitle(candidate?.searchTerm)
  if (searchTerm && names.includes(searchTerm) && best.confidence >= 0.66 && best.confidence < 0.88) {
    best = {
      confidence: Math.max(best.confidence, 0.88),
      relation: 'search-term',
      matchedName: searchTerm,
      reason: 'Selected from an exact search term',
    }
  }

  return best
}

function scoreFromEvidence(source, evidence) {
  const sourceScore = source === 'igdb' ? 3 : 2
  return sourceScore + Math.round(evidence.confidence * 100)
}

function scoreCandidate(app, candidate) {
  return scoreFromEvidence(candidate?.source, assessCoverMatch(app, candidate))
}

function dedupeCandidates(candidates) {
  const seen = new Set()
  const result = []

  for (const candidate of candidates) {
    const key = candidate.saveUrl || candidate.url || candidate.key
    if (!key || seen.has(key)) continue
    seen.add(key)
    result.push(candidate)
  }

  return result
}

export function pickFallbackCoverCandidate(app, candidates) {
  if (!Array.isArray(candidates) || candidates.length === 0) return null
  return [...candidates].sort((a, b) => scoreCandidate(app, b) - scoreCandidate(app, a))[0]
}

function compactCandidate(candidate, index) {
  return {
    id: String(index),
    name: candidate.name,
    source: candidate.source,
    searchTerm: candidate.searchTerm,
  }
}

function buildCoverCacheKey(app, locale) {
  return coverCache.makeKey({
    locale,
    name: app?.name || '',
    originalName: app?.['original-name'] || '',
    canonicalName: app?.['canonical-name'] || '',
    searchTerms: getCoverSearchCandidates(app).slice(0, MAX_SEARCH_TERMS),
    platform: app?.['app-type'] || '',
  })
}

function buildCoverSelectionPrompt(locale = getCurrentLocale()) {
  const languageName = getPromptLanguageName(locale)

  return [
    'You select the best cover art match for a Sunshine game streaming library.',
    'Return only one valid JSON object, with no markdown.',
    buildLocalizedInstruction(locale),
    `Short reasons should be in ${languageName}.`,
    'Choose the candidate that best matches the target game, not DLC, soundtrack, tool, demo, or unrelated same-name software.',
    'Prefer official game entries and exact title matches.',
    'If candidates are equally plausible, prefer box-art/library style covers over wide headers.',
  ].join(' ')
}

async function collectCoverCandidates(app) {
  const searchTerms = getCoverSearchCandidates(app).slice(0, MAX_SEARCH_TERMS)
  const candidates = []

  for (const term of searchTerms) {
    const results = await searchAllCovers(term)
    const covers = [...(results.igdb || []), ...(results.steam || [])].slice(0, MAX_COVERS_PER_TERM)
    for (const cover of covers) {
      candidates.push({
        ...cover,
        searchTerm: term,
      })
    }
  }

  return dedupeCandidates(candidates)
    .map((candidate, index) => {
      const evidence = assessCoverMatch(app, candidate)
      return {
        ...candidate,
        matchConfidence: evidence.confidence,
        matchRelation: evidence.relation,
        matchReason: evidence.reason,
        fallbackScore: scoreFromEvidence(candidate.source, evidence),
        originalIndex: index,
      }
    })
    .sort((a, b) => b.fallbackScore - a.fallbackScore)
    .slice(0, MAX_AI_CANDIDATES)
}

export function calibrateCoverConfidence(app, candidate, aiConfidence = 0, aiReason = '') {
  const evidence = assessCoverMatch(app, candidate)
  const modelConfidence = clamp01(aiConfidence)
  const evidenceConfidence = evidence.confidence

  let confidence = modelConfidence || evidenceConfidence
  if (evidenceConfidence >= 0.9) {
    confidence = Math.max(confidence, evidenceConfidence)
  } else if (modelConfidence > 0) {
    confidence = Math.min(modelConfidence, evidenceConfidence + 0.18)
  }

  return {
    ...candidate,
    aiCoverConfidence: clamp01(confidence),
    aiCoverReason: aiReason || evidence.reason,
    coverMatchConfidence: evidenceConfidence,
    coverMatchRelation: evidence.relation,
    coverMatchReason: evidence.reason,
  }
}

async function askAiToPickCover(app, candidates) {
  const locale = getCurrentLocale()
  const controller = new AbortController()
  const timeoutId = setTimeout(() => controller.abort(), AI_COVER_SELECTION_TIMEOUT_MS)
  let response

  try {
    response = await fetch(API_ENDPOINTS.AI_CHAT_COMPLETIONS, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      signal: controller.signal,
      body: JSON.stringify({
        messages: [
          { role: 'system', content: buildCoverSelectionPrompt(locale) },
          {
            role: 'user',
            content: JSON.stringify({
              task: 'select_best_cover',
              locale,
              target: {
                name: app?.name || '',
                originalName: app?.['original-name'] || '',
                canonicalName: app?.['canonical-name'] || '',
                searchTerms: getCoverSearchCandidates(app),
                platform: app?.['app-type'] || '',
              },
              candidates: candidates.map(compactCandidate),
              outputSchema: {
                selectedId: 'id string from candidates',
                confidence: 0.0,
                reason: 'short user-facing reason',
              },
            }),
          },
        ],
        temperature: 0.1,
        max_tokens: 1024,
      }),
    })
  } finally {
    clearTimeout(timeoutId)
  }

  const data = await response.json().catch(() => ({}))
  if (!response.ok) {
    const message = typeof data.error === 'string' ? data.error : data.error?.message
    throw new Error(message || `AI cover selection failed: ${response.status}`)
  }

  const content = data.choices?.[0]?.message?.content || ''
  const start = content.indexOf('{')
  const end = content.lastIndexOf('}')
  if (start === -1 || end === -1 || end <= start) {
    throw new Error('AI cover selection response did not contain JSON')
  }

  const parsed = JSON.parse(content.slice(start, end + 1))
  const selected = candidates[Number(parsed.selectedId)]
  return selected ? calibrateCoverConfidence(app, selected, parsed.confidence, parsed.reason || '') : null
}

export async function findBestCoverForApp(app) {
  const locale = getCurrentLocale()
  const cacheKey = buildCoverCacheKey(app, locale)
  const cached = coverCache.get(cacheKey)
  if (cached !== undefined) {
    return cached
  }

  let candidates = []
  try {
    candidates = await collectCoverCandidates(app)
  } catch (error) {
    console.warn('Cover candidate search failed; skipping cover selection:', error)
    return null
  }
  if (candidates.length === 0) {
    coverCache.set(cacheKey, null)
    return null
  }
  if (candidates.length === 1) {
    const selected = calibrateCoverConfidence(app, candidates[0])
    coverCache.set(cacheKey, selected)
    return selected
  }

  try {
    const aiSelected = await askAiToPickCover(app, candidates)
    const fallback = aiSelected ? null : pickFallbackCoverCandidate(app, candidates)
    const selected = aiSelected || (fallback ? calibrateCoverConfidence(app, fallback) : null)
    coverCache.set(cacheKey, selected)
    return selected
  } catch (error) {
    console.warn('AI cover selection failed; using fallback cover candidate:', error)
    const fallback = pickFallbackCoverCandidate(app, candidates)
    const selected = fallback ? calibrateCoverConfidence(app, fallback) : null
    coverCache.set(cacheKey, selected)
    return selected
  }
}
