#!/usr/bin/env node
import fs from 'fs'
import path from 'path'
import process from 'process'

const root = process.cwd()
const registryDir = path.join(root, 'plugin-registry')
const pluginsDir = path.join(registryDir, 'plugins')
const indexPath = path.join(registryDir, 'index.json')
const blocklistPath = path.join(registryDir, 'blocklist.json')

const visibleStatuses = new Set(['listed', 'deprecated', 'blocked'])
const allowedStatuses = new Set(['draft', 'listed', 'deprecated', 'unlisted', 'blocked'])
const idPattern = /^[a-z0-9]+(\.[a-z0-9][a-z0-9-]*){2,}$/
const capabilityPattern = /^[a-z0-9][a-z0-9.-]*$/
const sha256Pattern = /^[a-fA-F0-9]{64}$/
const platforms = new Set(['windows', 'linux', 'macos'])
const channels = new Set(['stable', 'beta', 'nightly'])

const args = new Set(process.argv.slice(2))
const checkOnly = args.has('--check')
const deterministic = checkOnly || args.has('--deterministic')

const readJson = (file) => {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch (err) {
    throw new Error(`${file}: ${err.message}`)
  }
}

const requireString = (obj, key, errors, context) => {
  if (typeof obj[key] !== 'string' || obj[key].trim() === '') {
    errors.push(`${context}: ${key} must be a non-empty string`)
    return ''
  }
  return obj[key]
}

const requireArray = (obj, key, errors, context) => {
  if (!Array.isArray(obj[key])) {
    errors.push(`${context}: ${key} must be an array`)
    return []
  }
  return obj[key]
}

const validateUrl = (value, key, errors, context, githubOnly = false) => {
  if (typeof value !== 'string' || value.trim() === '') return
  let parsed
  try {
    parsed = new URL(value)
  } catch {
    errors.push(`${context}: ${key} must be a valid URL`)
    return
  }
  if (parsed.protocol !== 'https:') {
    errors.push(`${context}: ${key} must use https`)
  }
  if (githubOnly && !['github.com', 'api.github.com', 'raw.githubusercontent.com'].includes(parsed.hostname)) {
    errors.push(`${context}: ${key} must point to GitHub`)
  }
}

const validateUniqueStrings = (values, key, pattern, allowed, errors, context) => {
  const seen = new Set()
  for (const value of values) {
    if (typeof value !== 'string' || value.trim() === '') {
      errors.push(`${context}: ${key} entries must be non-empty strings`)
      continue
    }
    if (pattern && !pattern.test(value)) {
      errors.push(`${context}: ${key} entry '${value}' has an invalid format`)
    }
    if (allowed && !allowed.has(value)) {
      errors.push(`${context}: ${key} entry '${value}' is not supported`)
    }
    if (seen.has(value)) {
      errors.push(`${context}: ${key} entry '${value}' is duplicated`)
    }
    seen.add(value)
  }
}

const latestRelease = (listing) => {
  if (!Array.isArray(listing.releases) || listing.releases.length === 0) {
    return null
  }
  return listing.releases.find((release) => release.channel === 'stable') || listing.releases[0]
}

const normalizeListing = (listing) => {
  const release = latestRelease(listing)
  return {
    id: listing.id,
    name: listing.name,
    summary: listing.summary,
    description: listing.description || '',
    publisher: listing.publisher,
    status: listing.status,
    reason: listing.reason || '',
    repo: listing.repo,
    homepage: listing.homepage || listing.repo,
    support_url: listing.support_url || listing.repo,
    license: listing.license || '',
    platforms: [...listing.platforms].sort(),
    capabilities: [...listing.capabilities].sort(),
    tags: Array.isArray(listing.tags) ? [...listing.tags].sort() : [],
    latest_release: release,
    releases: Array.isArray(listing.releases) ? listing.releases : [],
  }
}

const validateRelease = (release, errors, context) => {
  requireString(release, 'version', errors, context)
  const channel = requireString(release, 'channel', errors, context)
  if (channel && !channels.has(channel)) {
    errors.push(`${context}: channel '${channel}' is not supported`)
  }
  validateUrl(release.release_url, 'release_url', errors, context, true)
  validateUrl(release.attestation_url, 'attestation_url', errors, context, true)

  const assets = requireArray(release, 'assets', errors, context)
  for (const [index, asset] of assets.entries()) {
    const assetContext = `${context}.assets[${index}]`
    const platform = requireString(asset, 'platform', errors, assetContext)
    if (platform && !platforms.has(platform)) {
      errors.push(`${assetContext}: platform '${platform}' is not supported`)
    }
    requireString(asset, 'arch', errors, assetContext)
    validateUrl(requireString(asset, 'url', errors, assetContext), 'url', errors, assetContext, true)
    const sha256 = requireString(asset, 'sha256', errors, assetContext)
    if (sha256 && !sha256Pattern.test(sha256)) {
      errors.push(`${assetContext}: sha256 must be 64 hex characters`)
    }
    if (asset.size !== undefined && (!Number.isInteger(asset.size) || asset.size <= 0)) {
      errors.push(`${assetContext}: size must be a positive integer when present`)
    }
  }
}

const validateListing = (listing, file, seenIds, errors) => {
  const context = path.relative(root, file)
  if (listing.schema_version !== 1) {
    errors.push(`${context}: schema_version must be 1`)
  }

  const id = requireString(listing, 'id', errors, context)
  if (id && !idPattern.test(id)) {
    errors.push(`${context}: id must use reverse-DNS form`)
  }
  if (id && seenIds.has(id)) {
    errors.push(`${context}: duplicate plugin id '${id}'`)
  }
  seenIds.add(id)

  requireString(listing, 'name', errors, context)
  const summary = requireString(listing, 'summary', errors, context)
  if (summary.length > 180) {
    errors.push(`${context}: summary must be 180 characters or fewer`)
  }

  if (!listing.publisher || typeof listing.publisher !== 'object' || Array.isArray(listing.publisher)) {
    errors.push(`${context}: publisher must be an object`)
  } else {
    requireString(listing.publisher, 'name', errors, `${context}.publisher`)
    validateUrl(requireString(listing.publisher, 'url', errors, `${context}.publisher`), 'url', errors, `${context}.publisher`, true)
  }

  const status = requireString(listing, 'status', errors, context)
  if (status && !allowedStatuses.has(status)) {
    errors.push(`${context}: status '${status}' is not supported`)
  }
  if (status === 'blocked' && typeof listing.reason !== 'string') {
    errors.push(`${context}: blocked listings must include reason`)
  }

  validateUrl(requireString(listing, 'repo', errors, context), 'repo', errors, context, true)
  validateUrl(listing.homepage, 'homepage', errors, context)
  validateUrl(listing.support_url, 'support_url', errors, context)

  const listingPlatforms = requireArray(listing, 'platforms', errors, context)
  validateUniqueStrings(listingPlatforms, 'platforms', null, platforms, errors, context)

  const capabilities = requireArray(listing, 'capabilities', errors, context)
  validateUniqueStrings(capabilities, 'capabilities', capabilityPattern, null, errors, context)

  if (listing.tags !== undefined) {
    validateUniqueStrings(requireArray(listing, 'tags', errors, context), 'tags', null, null, errors, context)
  }

  if (listing.releases !== undefined) {
    if (!Array.isArray(listing.releases)) {
      errors.push(`${context}: releases must be an array`)
    } else {
      for (const [index, release] of listing.releases.entries()) {
        validateRelease(release, errors, `${context}.releases[${index}]`)
      }
    }
  }

  if (visibleStatuses.has(status) && (!Array.isArray(listing.releases) || listing.releases.length === 0)) {
    errors.push(`${context}: visible listings must include at least one release`)
  }
}

const validateBlocklist = (blocklist, errors) => {
  if (blocklist.schema_version !== 1) {
    errors.push('plugin-registry/blocklist.json: schema_version must be 1')
  }
  for (const key of ['blocked_plugins', 'blocked_versions']) {
    if (!Array.isArray(blocklist[key])) {
      errors.push(`plugin-registry/blocklist.json: ${key} must be an array`)
    }
  }
}

const pluginFiles = fs.existsSync(pluginsDir)
  ? fs.readdirSync(pluginsDir)
      .filter((name) => name.endsWith('.json'))
      .sort()
      .map((name) => path.join(pluginsDir, name))
  : []

const errors = []
const seenIds = new Set()
const listings = []

const blocklist = fs.existsSync(blocklistPath) ? readJson(blocklistPath) : { schema_version: 1, blocked_plugins: [], blocked_versions: [] }
validateBlocklist(blocklist, errors)

for (const file of pluginFiles) {
  const listing = readJson(file)
  validateListing(listing, file, seenIds, errors)
  if (visibleStatuses.has(listing.status)) {
    listings.push(normalizeListing(listing))
  }
}

if (errors.length > 0) {
  console.error(errors.map((error) => `- ${error}`).join('\n'))
  process.exit(1)
}

const index = {
  marketplace_version: 1,
  generated_at: deterministic ? null : new Date().toISOString(),
  source: 'https://github.com/AlkaidLab/sunshine-plugin-registry',
  blocklist,
  plugins: listings.sort((a, b) => a.id.localeCompare(b.id)),
}

const serialized = `${JSON.stringify(index, null, 2)}\n`

if (checkOnly) {
  const existing = fs.existsSync(indexPath) ? fs.readFileSync(indexPath, 'utf8') : ''
  if (existing !== serialized) {
    console.error('plugin-registry/index.json is out of date. Run node scripts/plugin-registry/build-index.js')
    process.exit(1)
  }
} else {
  fs.writeFileSync(indexPath, serialized)
}

console.log(`Plugin registry index ${checkOnly ? 'validated' : 'generated'} with ${listings.length} visible plugin(s).`)
