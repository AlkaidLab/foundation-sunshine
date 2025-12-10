#!/usr/bin/env node
/**
 * Reverse i18n Sync Script
 * 
 * This script identifies keys that exist in other locale files but are missing
 * from en.json (base file), and adds them to en.json.
 * 
 * Usage:
 *   node scripts/reverse-sync-i18n.js              # Dry run - show what would be added
 *   node scripts/reverse-sync-i18n.js --sync       # Actually add missing keys to en.json
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
 * Main reverse sync function
 */
function reverseSyncLocales() {
  console.log('🔍 Checking for keys in other locales that are missing from en.json...\n')
  
  // Read base locale
  const baseLocalePath = path.join(localeDir, baseLocale)
  if (!fs.existsSync(baseLocalePath)) {
    console.error(`❌ Base locale file not found: ${baseLocale}`)
    process.exit(1)
  }
  
  const baseContent = JSON.parse(fs.readFileSync(baseLocalePath, 'utf8'))
  const baseKeys = new Set(getAllKeys(baseContent))
  
  console.log(`📋 Base locale (${baseLocale}) has ${baseKeys.size} keys\n`)
  
  // Get all locale files
  const localeFiles = fs.readdirSync(localeDir)
    .filter(file => file.endsWith('.json') && file !== baseLocale)
    .sort()
  
  // Collect all keys from all locales
  const allKeysMap = new Map() // key -> { files: Set, sampleValue: string }
  
  for (const localeFile of localeFiles) {
    const localePath = path.join(localeDir, localeFile)
    const content = JSON.parse(fs.readFileSync(localePath, 'utf8'))
    const localeKeys = getAllKeys(content)
    
    for (const key of localeKeys) {
      if (!baseKeys.has(key)) {
        if (!allKeysMap.has(key)) {
          allKeysMap.set(key, {
            files: new Set(),
            sampleValue: getValue(content, key)
          })
        }
        allKeysMap.get(key).files.add(localeFile)
      }
    }
  }
  
  if (allKeysMap.size === 0) {
    console.log('✅ No missing keys found - en.json has all keys from other locales!\n')
    return
  }
  
  console.log(`⚠️  Found ${allKeysMap.size} keys in other locales but missing from ${baseLocale}:\n`)
  
  const sortedMissingKeys = Array.from(allKeysMap.entries()).sort((a, b) => a[0].localeCompare(b[0]))
  
  for (const [key, info] of sortedMissingKeys) {
    console.log(`  📍 ${key}`)
    console.log(`     Found in: ${Array.from(info.files).join(', ')}`)
    console.log(`     Sample: "${info.sampleValue}"`)
    console.log()
  }
  
  if (syncMode) {
    console.log('🔄 Adding missing keys to en.json...\n')
    
    let addedCount = 0
    for (const [key, info] of sortedMissingKeys) {
      // Use the sample value as placeholder (it's likely English or close to it)
      setValue(baseContent, key, info.sampleValue)
      addedCount++
      console.log(`   ✓ Added: ${key}`)
    }
    
    // Write updated base locale
    fs.writeFileSync(baseLocalePath, JSON.stringify(baseContent, null, 2) + '\n', 'utf8')
    
    console.log(`\n✅ Successfully added ${addedCount} keys to ${baseLocale}`)
    console.log('\n💡 Next steps:')
    console.log('   1. Review the added keys in en.json')
    console.log('   2. Update the values to proper English translations if needed')
    console.log('   3. Run: npm run i18n:sync to sync these keys to all locales')
    console.log('   4. Run: npm run i18n:format to ensure consistent formatting')
  } else {
    console.log('💡 This is a dry run. Use --sync flag to actually add these keys to en.json')
    console.log('   Command: node scripts/reverse-sync-i18n.js --sync')
  }
}

// Run reverse sync
reverseSyncLocales()
