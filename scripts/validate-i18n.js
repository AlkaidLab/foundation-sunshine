#!/usr/bin/env node
/**
 * i18n Translation Validation and Sync Script
 * 
 * This script validates that all locale files have the same keys as the base locale (en.json).
 * It can also automatically add missing keys with placeholder values.
 * 
 * Usage:
 *   node scripts/validate-i18n.js              # Validate only (report missing keys, exit with error code on failure)
 *   node scripts/validate-i18n.js --sync       # Auto-sync missing keys with English values
 */

import fs from 'fs'
import path from 'path'
import { fileURLToPath } from 'url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

const localeDir = path.join(__dirname, '../src_assets/common/assets/web/public/assets/locale')
const baseLocale = 'en.json'

// Parse command line arguments
const args = process.argv.slice(2)
const syncMode = args.includes('--sync')

/**
 * Get all keys from a nested object
 */
function getAllKeys(obj, prefix = '') {
  const keys = []
  for (const key in obj) {
    const fullKey = prefix ? `${prefix}.${key}` : key
    if (typeof obj[key] === 'object' && obj[key] !== null && !Array.isArray(obj[key])) {
      keys.push(...getAllKeys(obj[key], fullKey))
    } else {
      keys.push(fullKey)
    }
  }
  return keys
}

/**
 * Get value from nested object using dot notation
 */
function getValue(obj, path) {
  return path.split('.').reduce((current, key) => current?.[key], obj)
}

/**
 * Set value in nested object using dot notation
 */
function setValue(obj, path, value) {
  const keys = path.split('.')
  const lastKey = keys.pop()
  const target = keys.reduce((current, key) => {
    if (!current[key] || typeof current[key] !== 'object') {
      current[key] = {}
    }
    return current[key]
  }, obj)
  target[lastKey] = value
}

/**
 * Remove a key from nested object using dot notation
 */
function removeKey(obj, keyPath) {
  const keys = keyPath.split('.')
  const lastKey = keys.pop()
  const target = keys.reduce((current, key) => {
    if (!current || !current[key] || typeof current[key] !== 'object') {
      return null
    }
    return current[key]
  }, obj)
  
  if (target && target.hasOwnProperty(lastKey)) {
    delete target[lastKey]
    // Clean up empty objects
    if (Object.keys(target).length === 0 && keys.length > 0) {
      const parent = keys.reduce((current, key) => {
        if (!current || !current[key] || typeof current[key] !== 'object') {
          return null
        }
        return current[key]
      }, obj)
      if (parent && parent[keys[keys.length - 1]]) {
        delete parent[keys[keys.length - 1]]
      }
    }
    return true
  }
  return false
}

/**
 * Sort object keys recursively
 */
function sortObjectKeys(obj) {
  if (typeof obj !== 'object' || obj === null || Array.isArray(obj)) {
    return obj
  }
  
  const sorted = {}
  const keys = Object.keys(obj).sort()
  
  for (const key of keys) {
    sorted[key] = sortObjectKeys(obj[key])
  }
  
  return sorted
}

/**
 * Get list of keys that should remain in English (technical terms, protocols, etc.)
 * These keys will be automatically overwritten with English values in sync mode
 */
function getEnglishOnlyKeys() {
  return [
    "address_family_both", // IPv4+IPv6
    "port_tcp", // TCP
    "port_udp", // UDP
    "scan_result_filter_url", // URL
    "webhook_url", // Webhook URL (URL is technical term)
    "audio_sink_placeholder_macos", // BlackHole 2ch (product name)
    "virtual_sink_placeholder", // Steam Streaming Speakers (product name)
    "gamepad_ds4", // DS4 (PS4) - product name
    "gamepad_ds5", // DS5 (PS5) - product name
    "gamepad_switch", // Nintendo Pro (Switch) - product name
    "gamepad_x360", // X360 (Xbox 360) - product name
    "gamepad_xone", // XOne (Xbox One) - product name
    "port_web_ui", // Web UI
    "boom_sunshine", // Boom!
    "boom_sunshine_title", // Boom!
    "boom_sunshine_button", // Boom!
    "boom_sunshine_button_desc", // Boom!
    "boom_sunshine_button_title", // Boom!
    "boom_sunshine_button_desc", // Boom!
    "upnp", // UPnP
    "scan_result_type_url", // URL
    "scan_result_filter_url_title", // URL
    "adapter_name_placeholder_windows", // Radeon RX 580 Series
    "gamepad_mode_x360", // Xbox 360 product name
    "gamepad_mode_ds4", // DualShock 4 product name
    "crown_edition_desc", // Publisher/platform attribution
    "moonlight_ohos", // Project name
    "moonlight_pc_title", // Project name
    "notifications.webhook.test_payload.event_type_label",
    "notifications.webhook.test_payload.heading",
    "notifications.webhook.test_payload.hostname_label",
    "notifications.webhook.test_payload.result",
    "notifications.webhook.test_payload.result_label",
    "notifications.webhook.test_payload.sample_application",
    "notifications.webhook.test_payload.sample_application_label",
    "notifications.webhook.test_payload.sample_client",
    "notifications.webhook.test_payload.sample_client_label",
    "notifications.webhook.test_payload.sample_stream",
    "notifications.webhook.test_payload.sample_stream_label",
    "notifications.webhook.test_payload.time_label",
    "notifications.webhook.test_payload.title",
  ]
}

function isEnglishOnlyKey(key, englishOnlyKeys = getEnglishOnlyKeys()) {
  return englishOnlyKeys.includes(key) || englishOnlyKeys.includes(key.split('.').pop())
}

function isChineseLocale(localeFile) {
  return localeFile === 'zh.json' || localeFile === 'zh_TW.json'
}

function isChineseWebhookPayloadKey(key, localeFile) {
  return isChineseLocale(localeFile) &&
         key.startsWith('notifications.webhook.test_payload.')
}

function shouldForceEnglish(key, localeFile, englishOnlyKeys = getEnglishOnlyKeys()) {
  return !isChineseWebhookPayloadKey(key, localeFile) &&
         isEnglishOnlyKey(key, englishOnlyKeys)
}

/**
 * Check if a value should be excluded from translation check
 * (e.g., technical terms, protocol names, product names that are commonly kept in English)
 */
function shouldSkipTranslationCheck(key, value, localeFile) {
  if (!value || typeof value !== 'string') {
    return false
  }
  
  const englishOnlyKeys = getEnglishOnlyKeys()
  
  // Check if key is in skip list
  if (shouldForceEnglish(key, localeFile, englishOnlyKeys)) {
    return true
  }
  
  // Check if value is a pure technical term (all uppercase, contains numbers/special chars)
  // Examples: "IPv4+IPv6", "TCP", "UDP", "URL", "DS4 (PS4)"
  const isTechnicalTerm = /^[A-Z0-9+\-()\s]+$/.test(value.trim()) && 
                          value.length < 50 && 
                          /[A-Z]/.test(value)
  
  // Check if value contains only product names or technical abbreviations
  const isProductName = /^(?:DS4(?: \(PS4\))?|DS5(?: \(PS5\))?|X360(?: \(Xbox 360\))?|XOne(?: \(Xbox One\))?|Xbox 360|DualShock 4|Nintendo Pro \(Switch\)|Steam Streaming Speakers|BlackHole 2ch|Foundation Sunshine|Sunshine|AlkaidLab|Moonlight(?: PC)?|ZakoHDR|ZakoVDD|ViGEmBus|TCP|UDP|URL|IPv4\+IPv6|Webhook URL)$/i.test(value.trim())
  
  // Check if value ends with common technical terms that can stay in English
  const hasTechnicalSuffix = value.length <= 40 &&
                             !/[.!?]/.test(value) &&
                             /\b(URL|TCP|UDP|IPv4|IPv6|UI|API|HTTP|HTTPS|DS4|DS5|X360)\b/i.test(value)
  
  return isTechnicalTerm || isProductName || hasTechnicalSuffix
}

/**
 * Check for untranslated keys (keys that have the same value as the base locale)
 */
function findUntranslatedKeys(baseContent, localeContent, localeFile) {
  // Skip English variants
  if (localeFile === 'en_GB.json' || localeFile === 'en_US.json') {
    return []
  }
  
  const baseKeys = getAllKeys(baseContent)
  const untranslated = []
  
  for (const key of baseKeys) {
    const baseValue = getValue(baseContent, key)
    const localeValue = getValue(localeContent, key)
    
    // Check if the value is the same as the base (untranslated)
    if (localeValue !== null && localeValue === baseValue) {
      // Skip if this key/value should not be checked for translation
      if (!shouldSkipTranslationCheck(key, localeValue, localeFile)) {
        untranslated.push(key)
      }
    }
  }
  
  return untranslated
}

/**
 * Find translations that changed runtime interpolation tokens or inline HTML.
 * Missing placeholders break messages at runtime, while lost tags can change
 * the rendering contract of v-html call sites.
 */
function findTokenMismatches(baseContent, localeContent, tokenPattern) {
  const mismatches = []

  for (const key of getAllKeys(baseContent)) {
    const baseValue = getValue(baseContent, key)
    const localeValue = getValue(localeContent, key)
    if (typeof baseValue !== 'string' || typeof localeValue !== 'string') {
      continue
    }

    const baseTokens = (baseValue.match(tokenPattern) || []).sort()
    const localeTokens = (localeValue.match(tokenPattern) || []).sort()
    if (JSON.stringify(baseTokens) !== JSON.stringify(localeTokens)) {
      mismatches.push({ key, baseTokens, localeTokens })
    }
  }

  return mismatches
}

/**
 * Parse inline HTML tokens without discarding their order and verify that
 * opening and closing tags are properly nested.
 */
function getHtmlStructure(value) {
  const tokens = value.match(/<\/?[A-Za-z][^>]*>/g) || []
  const voidTags = new Set([
    'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input',
    'link', 'meta', 'param', 'source', 'track', 'wbr'
  ])
  const stack = []
  let valid = true

  for (const token of tokens) {
    const match = token.match(/^<\s*(\/?)\s*([A-Za-z][A-Za-z0-9:-]*)/)
    if (!match) {
      valid = false
      continue
    }

    const closing = match[1] === '/'
    const tagName = match[2].toLowerCase()
    const selfClosing = /\/\s*>$/.test(token)

    if (closing) {
      if (stack.pop() !== tagName) {
        valid = false
      }
    } else if (!selfClosing && !voidTags.has(tagName)) {
      stack.push(tagName)
    }
  }

  return { tokens, valid: valid && stack.length === 0 }
}

function findHtmlMismatches(baseContent, localeContent) {
  const mismatches = []

  for (const key of getAllKeys(baseContent)) {
    const baseValue = getValue(baseContent, key)
    const localeValue = getValue(localeContent, key)
    if (typeof baseValue !== 'string' || typeof localeValue !== 'string') {
      continue
    }

    const baseHtml = getHtmlStructure(baseValue)
    const localeHtml = getHtmlStructure(localeValue)
    if (!baseHtml.valid ||
        !localeHtml.valid ||
        JSON.stringify(baseHtml.tokens) !== JSON.stringify(localeHtml.tokens)) {
      mismatches.push({
        key,
        baseTokens: baseHtml.tokens,
        localeTokens: localeHtml.tokens
      })
    }
  }

  return mismatches
}

/**
 * The backend emits Chinese Webhook payload text only for zh and zh_TW.
 * Every other locale must use the English payload verbatim.
 */
function findWebhookLanguageMismatches(baseContent, localeContent, localeFile) {
  const mismatches = []
  const chineseLocale = isChineseLocale(localeFile)

  for (const key of getAllKeys(baseContent)) {
    if (!key.startsWith('notifications.webhook.test_payload.')) {
      continue
    }

    const baseValue = getValue(baseContent, key)
    const localeValue = getValue(localeContent, key)
    if (typeof baseValue !== 'string' || typeof localeValue !== 'string') {
      continue
    }

    const matchesContract = chineseLocale
      ? /\p{Script=Han}/u.test(localeValue)
      : localeValue === baseValue
    if (!matchesContract) {
      mismatches.push({ key, expected: chineseLocale ? 'Chinese' : 'English' })
    }
  }

  return mismatches
}

/**
 * Main validation function
 */
function validateLocales() {
  console.log('🔍 Validating i18n translations...\n')
  
  // Read base locale
  const baseLocalePath = path.join(localeDir, baseLocale)
  if (!fs.existsSync(baseLocalePath)) {
    console.error(`❌ Base locale file not found: ${baseLocale}`)
    process.exit(1)
  }
  
  const baseContent = JSON.parse(fs.readFileSync(baseLocalePath, 'utf8'))
  const baseKeys = getAllKeys(baseContent).sort()
  
  console.log(`📋 Base locale (${baseLocale}) has ${baseKeys.length} keys\n`)
  
  // Get all locale files
  const localeFiles = fs.readdirSync(localeDir)
    .filter(file => file.endsWith('.json') && file !== baseLocale)
    .sort()
  
  let hasErrors = false
  let hasParseErrors = false
  const results = []
  
  for (const localeFile of localeFiles) {
    const localePath = path.join(localeDir, localeFile)
    let content
    
    try {
      content = JSON.parse(fs.readFileSync(localePath, 'utf8'))
    } catch (e) {
      console.error(`❌ Failed to parse ${localeFile}: ${e.message}`)
      hasErrors = true
      hasParseErrors = true
      continue
    }
    
    const localeKeys = getAllKeys(content).sort()
    const missingKeys = baseKeys.filter(key => !localeKeys.includes(key))
    const extraKeys = localeKeys.filter(key => !baseKeys.includes(key))
    const untranslatedKeys = findUntranslatedKeys(baseContent, content, localeFile)
    const placeholderMismatches = findTokenMismatches(baseContent, content, /\{[^{}]+\}/g)
    const htmlMismatches = findHtmlMismatches(baseContent, content)
    const webhookLanguageMismatches = findWebhookLanguageMismatches(baseContent, content, localeFile)
    
    // Matching English text is only a heuristic: product names, technical terms,
    // loanwords, and short labels can legitimately be identical across locales.
    // Keep reporting these entries for translator review, but only structural
    // key drift should fail the validation command.
    const hasIssues = missingKeys.length > 0 ||
                      extraKeys.length > 0 ||
                      placeholderMismatches.length > 0 ||
                      htmlMismatches.length > 0 ||
                      webhookLanguageMismatches.length > 0
    
    if (!hasIssues) {
      if (untranslatedKeys.length > 0) {
        console.log(`⚠️  ${localeFile}: All keys present; ${untranslatedKeys.length} values match English and need review (${localeKeys.length} keys)`)
      } else {
        console.log(`✅ ${localeFile}: All keys present and translated (${localeKeys.length} keys)`)
      }
      results.push({
        file: localeFile,
        status: 'ok',
        missing: 0,
        extra: 0,
        placeholderMismatches: 0,
        htmlMismatches: 0,
        webhookLanguageMismatches: 0,
        manualWebhookLanguageMismatches: 0,
        manualMissing: 0,
        untranslated: untranslatedKeys.length
      })
      
      // Still overwrite English-only keys even if no other issues
      if (syncMode) {
        let modified = false
        const englishOnlyKeys = getEnglishOnlyKeys()
        let overwrittenCount = 0
        for (const key of baseKeys) {
          if (shouldForceEnglish(key, localeFile, englishOnlyKeys)) {
            const baseValue = getValue(baseContent, key)
            const currentValue = getValue(content, key)
            if (currentValue !== baseValue) {
              setValue(content, key, baseValue)
              overwrittenCount++
              modified = true
            }
          }
        }
        if (overwrittenCount > 0) {
          console.log(`   🔄 Overwritten ${overwrittenCount} English-only keys with English values`)
        }
        // Always sort and write in sync mode, even if no changes were made
        const sorted = sortObjectKeys(content)
        const formatted = JSON.stringify(sorted, null, 2) + '\n'
        const original = fs.readFileSync(localePath, 'utf8')
        
        // Always write in sync mode to ensure consistent formatting
        // Compare to detect if actual changes were made
        let originalParsed
        try {
          originalParsed = JSON.parse(original)
        } catch (e) {
          originalParsed = null
        }
        
        const keysChanged = originalParsed ? JSON.stringify(originalParsed) !== JSON.stringify(sorted) : true
        const formatChanged = original.trim() !== formatted.trim()
        
        // Always write to ensure consistent formatting
        fs.writeFileSync(localePath, formatted, 'utf8')
        if (!modified) {
          if (keysChanged) {
            console.log(`   🔄 Sorted keys alphabetically`)
          } else if (formatChanged) {
            console.log(`   🔄 Reformatted file`)
          } else {
            // Even if no changes, we still write to ensure consistency
            console.log(`   ✓ File is properly sorted and formatted`)
          }
        }
      }
    } else {
      hasErrors = true
      console.log(`❌ ${localeFile}: Issues found`)
      
      if (missingKeys.length > 0) {
        console.log(`   Missing ${missingKeys.length} keys:`)
        missingKeys.slice(0, 5).forEach(key => console.log(`     - ${key}`))
        if (missingKeys.length > 5) {
          console.log(`     ... and ${missingKeys.length - 5} more`)
        }
      }
      
      if (extraKeys.length > 0) {
        console.log(`   Extra ${extraKeys.length} keys (not in base):`)
        extraKeys.slice(0, 5).forEach(key => console.log(`     - ${key}`))
        if (extraKeys.length > 5) {
          console.log(`     ... and ${extraKeys.length - 5} more`)
        }
      }

      if (placeholderMismatches.length > 0) {
        console.log(`   Placeholder mismatches in ${placeholderMismatches.length} keys:`)
        placeholderMismatches.slice(0, 5).forEach(({ key, baseTokens, localeTokens }) => {
          console.log(`     - ${key}: expected [${baseTokens.join(', ')}], found [${localeTokens.join(', ')}]`)
        })
        if (placeholderMismatches.length > 5) {
          console.log(`     ... and ${placeholderMismatches.length - 5} more`)
        }
      }

      if (htmlMismatches.length > 0) {
        console.log(`   Inline HTML mismatches in ${htmlMismatches.length} keys:`)
        htmlMismatches.slice(0, 5).forEach(({ key, baseTokens, localeTokens }) => {
          console.log(`     - ${key}: expected [${baseTokens.join(', ')}], found [${localeTokens.join(', ')}]`)
        })
        if (htmlMismatches.length > 5) {
          console.log(`     ... and ${htmlMismatches.length - 5} more`)
        }
      }

      if (webhookLanguageMismatches.length > 0) {
        console.log(`   Webhook language mismatches in ${webhookLanguageMismatches.length} keys:`)
        webhookLanguageMismatches.slice(0, 5).forEach(({ key, expected }) => {
          console.log(`     - ${key}: expected ${expected}`)
        })
        if (webhookLanguageMismatches.length > 5) {
          console.log(`     ... and ${webhookLanguageMismatches.length - 5} more`)
        }
      }
      
      if (untranslatedKeys.length > 0) {
        console.log(`   ⚠️  ${untranslatedKeys.length} untranslated keys (same as English):`)
        untranslatedKeys.slice(0, 10).forEach(key => {
          const value = getValue(content, key)
          const displayValue = value && value.length > 50 ? value.substring(0, 50) + '...' : value
          console.log(`     - ${key}: "${displayValue}"`)
        })
        if (untranslatedKeys.length > 10) {
          console.log(`     ... and ${untranslatedKeys.length - 10} more`)
        }
      }
      
      results.push({ 
        file: localeFile, 
        status: 'error', 
        missing: missingKeys.length, 
        extra: extraKeys.length,
        placeholderMismatches: placeholderMismatches.length,
        htmlMismatches: htmlMismatches.length,
        webhookLanguageMismatches: webhookLanguageMismatches.length,
        manualWebhookLanguageMismatches: isChineseLocale(localeFile) ? webhookLanguageMismatches.length : 0,
        manualMissing: 0,
        untranslated: untranslatedKeys.length,
        missingKeys,
        untranslatedKeys,
        content
      })
      
      // Auto-sync if requested
      if (syncMode) {
        let modified = false
        const englishOnlyKeys = getEnglishOnlyKeys()
        const manualMissingKeys = []
        
        // Add missing keys
        if (missingKeys.length > 0) {
          console.log(`   🔄 Syncing missing keys...`)
          for (const key of missingKeys) {
            if (isChineseWebhookPayloadKey(key, localeFile) &&
                !shouldForceEnglish(key, localeFile, englishOnlyKeys)) {
              manualMissingKeys.push(key)
              continue
            }
            const baseValue = getValue(baseContent, key)
            setValue(content, key, baseValue)
          }
          const addedCount = missingKeys.length - manualMissingKeys.length
          if (addedCount > 0) {
            console.log(`   ✓ Added ${addedCount} missing keys with English values`)
            modified = true
          }
          if (manualMissingKeys.length > 0) {
            console.log(`   ⚠️  Left ${manualMissingKeys.length} Chinese Webhook keys for manual translation`)
          }
        }
        
        // Remove extra keys
        if (extraKeys.length > 0) {
          console.log(`   🗑️  Removing extra keys...`)
          for (const key of extraKeys) {
            removeKey(content, key)
          }
          console.log(`   ✓ Removed ${extraKeys.length} extra keys`)
          modified = true
        }
        
        // Overwrite English-only keys with English values (force overwrite even if different)
        let overwrittenCount = 0
        for (const key of baseKeys) {
          if (shouldForceEnglish(key, localeFile, englishOnlyKeys)) {
            const baseValue = getValue(baseContent, key)
            const currentValue = getValue(content, key)
            // Force overwrite English-only keys with English values
            if (currentValue !== baseValue) {
              setValue(content, key, baseValue)
              overwrittenCount++
            }
          }
        }
        if (overwrittenCount > 0) {
          console.log(`   🔄 Overwritten ${overwrittenCount} English-only keys with English values`)
          modified = true
        }
        
        if (modified) {
          // Sort keys before writing
          const sorted = sortObjectKeys(content)
          fs.writeFileSync(localePath, JSON.stringify(sorted, null, 2) + '\n', 'utf8')
        }

        results[results.length - 1].manualMissing = manualMissingKeys.length
      }
    }
    console.log()
  }
  
  // Summary
  console.log('━'.repeat(60))
  console.log('📊 Summary:')
  console.log(`   Total locales checked: ${localeFiles.length}`)
  console.log(`   Locales with all keys: ${results.filter(r => r.status === 'ok').length}`)
  console.log(`   Locales with issues: ${results.filter(r => r.status === 'error').length}`)
  
  const totalUntranslated = results.reduce((sum, r) => sum + (r.untranslated || 0), 0)
  if (totalUntranslated > 0) {
    console.log(`   ℹ️  Values matching English (review only): ${totalUntranslated}`)
  }
  
  if (syncMode) {
    const synced = results.filter(r =>
      r.status === 'error' && r.missing > (r.manualMissing || 0)
    )
    if (synced.length > 0) {
      console.log(`\n✅ Synced ${synced.length} locale files with missing keys`)
      console.log('   ⚠️  Remember to translate the English placeholder values!')
    }
  }
  
  console.log('━'.repeat(60))
  
  const hasManualErrors = results.some(r =>
    (r.placeholderMismatches || 0) > 0 ||
    (r.htmlMismatches || 0) > 0 ||
    (r.manualWebhookLanguageMismatches || 0) > 0 ||
    (r.manualMissing || 0) > 0
  )
  if (hasParseErrors || hasManualErrors || (hasErrors && !syncMode)) {
    if (!syncMode) {
      console.log('\n💡 Tip: Run with --sync flag to automatically add missing keys')
    } else {
      console.log('\n💡 Parse, placeholder, inline HTML, and Chinese Webhook errors require manual fixes')
    }
    console.error('\n❌ Validation failed')
    process.exit(1)
  }
}

// Run validation
validateLocales()
