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
  ]
}

/**
 * Check if a value should be excluded from translation check
 * (e.g., technical terms, protocol names, product names that are commonly kept in English)
 */
function shouldSkipTranslationCheck(key, value) {
  if (!value || typeof value !== 'string') {
    return false
  }
  
  const englishOnlyKeys = getEnglishOnlyKeys()
  
  // Check if key is in skip list
  if (englishOnlyKeys.includes(key.split('.').pop())) {
    return true
  }
  
  // Check if value is a pure technical term (all uppercase, contains numbers/special chars)
  // Examples: "IPv4+IPv6", "TCP", "UDP", "URL", "DS4 (PS4)"
  const isTechnicalTerm = /^[A-Z0-9+\-()\s]+$/.test(value.trim()) && 
                          value.length < 50 && 
                          /[A-Z]/.test(value)
  
  // Check if value contains only product names or technical abbreviations
  const isProductName = /^(DS4|DS5|X360|Nintendo|Steam|BlackHole|TCP|UDP|URL|IPv4|IPv6|Webhook URL)/i.test(value.trim())
  
  // Check if value ends with common technical terms that can stay in English
  const hasTechnicalSuffix = /\b(URL|TCP|UDP|IPv4|IPv6|UI|API|HTTP|HTTPS|DS4|DS5|X360)\b/i.test(value)
  
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
      if (!shouldSkipTranslationCheck(key, localeValue)) {
        untranslated.push(key)
      }
    }
  }
  
  return untranslated
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
  const results = []
  
  for (const localeFile of localeFiles) {
    const localePath = path.join(localeDir, localeFile)
    let content
    
    try {
      content = JSON.parse(fs.readFileSync(localePath, 'utf8'))
    } catch (e) {
      console.error(`❌ Failed to parse ${localeFile}: ${e.message}`)
      hasErrors = true
      continue
    }
    
    const localeKeys = getAllKeys(content).sort()
    const missingKeys = baseKeys.filter(key => !localeKeys.includes(key))
    const extraKeys = localeKeys.filter(key => !baseKeys.includes(key))
    const untranslatedKeys = findUntranslatedKeys(baseContent, content, localeFile)
    
    const hasIssues = missingKeys.length > 0 || extraKeys.length > 0 || untranslatedKeys.length > 0
    
    if (!hasIssues) {
      console.log(`✅ ${localeFile}: All keys present and translated (${localeKeys.length} keys)`)
      results.push({ file: localeFile, status: 'ok', missing: 0, extra: 0, untranslated: 0 })
      
      // Still overwrite English-only keys even if no other issues
      if (syncMode) {
        let modified = false
        const englishOnlyKeys = getEnglishOnlyKeys()
        let overwrittenCount = 0
        for (const key of baseKeys) {
          const keyName = key.split('.').pop()
          if (englishOnlyKeys.includes(keyName)) {
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
        untranslated: untranslatedKeys.length,
        missingKeys,
        untranslatedKeys,
        content
      })
      
      // Auto-sync if requested
      if (syncMode) {
        let modified = false
        
        // Add missing keys
        if (missingKeys.length > 0) {
          console.log(`   🔄 Syncing missing keys...`)
          for (const key of missingKeys) {
            const baseValue = getValue(baseContent, key)
            setValue(content, key, baseValue)
          }
          console.log(`   ✓ Added ${missingKeys.length} missing keys with English values`)
          modified = true
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
        const englishOnlyKeys = getEnglishOnlyKeys()
        let overwrittenCount = 0
        for (const key of baseKeys) {
          const keyName = key.split('.').pop()
          if (englishOnlyKeys.includes(keyName)) {
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
    console.log(`   ⚠️  Total untranslated keys: ${totalUntranslated}`)
  }
  
  if (syncMode) {
    const synced = results.filter(r => r.status === 'error' && r.missing > 0)
    if (synced.length > 0) {
      console.log(`\n✅ Synced ${synced.length} locale files with missing keys`)
      console.log('   ⚠️  Remember to translate the English placeholder values!')
    }
  }
  
  console.log('━'.repeat(60))
  
  if (hasErrors && !syncMode) {
    console.log('\n💡 Tip: Run with --sync flag to automatically add missing keys')
    console.error('\n❌ Validation failed')
    process.exit(1)
  }
}

// Run validation
validateLocales()
